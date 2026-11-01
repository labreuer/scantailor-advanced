// scantailor-batch: run a project's processing stages without the GUI.
//
// Loads a .ScanTailor project, runs Fix Orientation, Split Pages, Deskew, Select Content and
// Margins (or the stages up to the one named) on every page, as the GUI's batch processing
// does, and writes the project back. With --through output it runs Output too, which writes
// each page's image into the project's output directory, as the GUI's Output stage does. Based on ConsoleBatch, which scantailor-advanced had
// until commit 74d90954 (2019), brought up to the current code.
//
// Each stage runs on all pages before the next starts (pages are processed in parallel
// within a stage), because Split Pages can turn one image into two pages. Pages keep any
// settings they already have; pages without them get the current default profile's, as
// the GUI's batch processing does. No windows are opened: Qt runs on its offscreen
// platform unless QT_QPA_PLATFORM says otherwise.

#include <config.h>
#include <version.h>

#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QString>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Application.h"
#include "ApplicationSettings.h"
#include "ColorSchemeFactory.h"
#include "ColorSchemeManager.h"
#include "FileNameDisambiguator.h"
#include "IconProvider.h"
#include "LoadFileTask.h"
#include "OutputFileNameGenerator.h"
#include "PageId.h"
#include "PageRange.h"
#include "PageSelectionAccessor.h"
#include "PageSelectionProvider.h"
#include "PageSequence.h"
#include "ProjectPages.h"
#include "ProjectReader.h"
#include "ProjectWriter.h"
#include "SelectedPage.h"
#include "StageSequence.h"
#include "StyledIconPack.h"
#include "Utils.h"
#include "filters/deskew/Task.h"
#include "filters/fix_orientation/Task.h"
#include "filters/output/Task.h"
#include "filters/page_layout/Task.h"
#include "filters/page_split/Task.h"
#include "filters/select_content/Task.h"

namespace {

// The stages this tool runs, by the names the project file uses for them.
const char* const kStageNames[] = {"fix-orientation", "page-split", "deskew", "select-content", "page-layout",
                                   "output"};
const int kStageCount = sizeof(kStageNames) / sizeof(kStageNames[0]);

void usage(std::FILE* out) {
  std::fprintf(out,
               "Usage: scantailor-batch [--through STAGE] [--jobs N] [-q] PROJECT [OUTPUT]\n"
               "\n"
               "Runs the processing stages on every page of the ScanTailor project PROJECT and\n"
               "writes the result to OUTPUT (default: PROJECT itself).\n"
               "\n"
               "  --through STAGE  last stage to run: fix-orientation, page-split, deskew,\n"
               "                   select-content, page-layout (the default; Margins in the GUI)\n"
               "                   or output (also writes the pages into the output directory)\n"
               "  --jobs N         pages processed at once (default: the number of CPUs)\n"
               "  -q               no progress on stderr\n");
}

// StageSequence wants a PageSelectionAccessor; only the GUI's "apply to..." dialogs use it.
class NoSelection : public PageSelectionProvider {
 public:
  explicit NoSelection(std::shared_ptr<ProjectPages> pages) : m_pages(std::move(pages)) {}

  PageSequence allPages() const override { return m_pages->toPageSequence(PAGE_VIEW); }

  std::set<PageId> selectedPages() const override { return {}; }

  std::vector<PageRange> selectedRanges() const override { return {}; }

 private:
  std::shared_ptr<ProjectPages> m_pages;
};

class Batch {
 public:
  Batch(const QString& projectFile, int jobs, bool quiet) : m_jobs(jobs), m_quiet(quiet) {
    QFile file(projectFile);
    if (!file.open(QIODevice::ReadOnly)) {
      throw std::runtime_error("can't open " + projectFile.toStdString());
    }
    QDomDocument doc;
    if (!doc.setContent(&file)) {
      throw std::runtime_error(projectFile.toStdString() + " isn't a valid XML file");
    }
    file.close();

    // Version 3 is what scantailor-advanced wrote from 2018 through 1.1.3, and what
    // ScanAnalyze writes. 1.2.0 bumped it to 4 without changing how older data is read
    // (deskew's new oblique-mode falls back to autoOblique), but refuses 3 outright, so
    // read a 3 as a 4. The project is written back as 4, as the GUI would.
    QDomElement root = doc.documentElement();
    if (root.attribute("version") == "3" && PROJECT_VERSION == 4) {
      root.setAttribute("version", QString::number(PROJECT_VERSION));
      if (!quiet) {
        std::fprintf(stderr, "Reading a version 3 project as version %d.\n", PROJECT_VERSION);
      }
    }

    m_reader = std::make_unique<ProjectReader>(doc, QFileInfo(projectFile).absoluteFilePath());
    if (!m_reader->success()) {
      throw std::runtime_error(projectFile.toStdString()
                               + " can't be read: not a ScanTailor project, or one of another version");
    }
    m_pages = m_reader->pages();
    if (!m_pages->validateDpis()) {
      throw std::runtime_error("some images have no usable DPI; open the project in the GUI to fix them");
    }

    // What MainWindow::switchToNewProject does, minus the windows.
    const QString outDir = m_reader->outputDirectory();
    if (!outDir.isEmpty()) {
      core::Utils::maybeCreateCacheDir(outDir);
    }
    m_outFileNameGen = OutputFileNameGenerator(m_reader->namingDisambiguator(), outDir, m_pages->layoutDirection());
    for (const PageInfo& page : m_pages->toPageSequence(IMAGE_VIEW)) {
      m_outFileNameGen.disambiguator()->registerFile(page.imageId().filePath());
    }
    m_stages = std::make_shared<StageSequence>(
        m_pages, PageSelectionAccessor(std::make_shared<NoSelection>(m_pages)));
    m_reader->readFilterSettings(m_stages->filters());
    if (!outDir.isEmpty()) {
      m_thumbnailCache = core::Utils::createThumbnailCache(m_outFileNameGen.outDir());
    }
  }

  // Runs stages 0..lastStage; returns the number of pages that failed.
  int process(int lastStage) {
    int failures = 0;
    for (int stage = 0; stage <= lastStage; ++stage) {
      // Recomputed for each stage, since Split Pages may have changed the pages.
      const PageSequence sequence = m_pages->toPageSequence(PAGE_VIEW);
      std::vector<PageInfo> pages(sequence.begin(), sequence.end());
      for (const PageInfo& page : pages) {
        for (int i = 0; i < m_stages->count(); ++i) {
          m_stages->filterAt(i)->loadDefaultSettings(page);
        }
      }
      failures = runStage(stage, pages);
    }
    return failures;
  }

  void save(const QString& path) const {
    const PageSequence sequence = m_pages->toPageSequence(PAGE_VIEW);
    SelectedPage selected = m_reader->selectedPage();
    if (selected.isNull() && sequence.numPages() > 0) {
      selected = SelectedPage(sequence.pageAt(0).id(), PAGE_VIEW);
    }
    ProjectWriter writer(m_pages, selected, m_outFileNameGen);
    if (!writer.write(path, m_stages->filters())) {
      throw std::runtime_error("can't write " + path.toStdString());
    }
  }

 private:
  int runStage(int stage, const std::vector<PageInfo>& pages) {
    std::atomic<size_t> next{0};
    std::atomic<size_t> done{0};
    std::mutex mutex;
    std::vector<std::string> errors;

    auto worker = [&]() {
      for (size_t i; (i = next++) < pages.size();) {
        const PageInfo& page = pages[i];
        try {
          BackgroundTaskPtr task = createCompositeTask(page, stage);
          FilterResultPtr result = (*task)();
          // In batch mode a result only refreshes thumbnails, so it is dropped, except
          // that a result without a filter means the image couldn't be loaded.
          if (!result || !result->filter()) {
            std::lock_guard<std::mutex> lock(mutex);
            errors.push_back(page.imageId().filePath().toStdString() + ": can't load the image");
          }
        } catch (const std::exception& e) {
          std::lock_guard<std::mutex> lock(mutex);
          errors.push_back(page.imageId().filePath().toStdString() + ": " + e.what());
        }
        const size_t n = ++done;
        if (!m_quiet) {
          std::lock_guard<std::mutex> lock(mutex);
          std::fprintf(stderr, "\r%-15s %zu/%zu pages", kStageNames[stage], n, pages.size());
          std::fflush(stderr);
        }
      }
    };

    const int jobs = std::max(1, std::min<int>(m_jobs, static_cast<int>(pages.size())));
    std::vector<std::thread> threads;
    for (int j = 0; j < jobs; ++j) {
      threads.emplace_back(worker);
    }
    for (std::thread& t : threads) {
      t.join();
    }
    if (!m_quiet) {
      std::fprintf(stderr, "\n");
    }
    for (const std::string& e : errors) {
      std::fprintf(stderr, "  ERROR %s\n", e.c_str());
    }
    return static_cast<int>(errors.size());
  }

  // MainWindow::createCompositeTask, through lastStage, in batch mode.
  BackgroundTaskPtr createCompositeTask(const PageInfo& page, int lastStage) {
    const bool batch = true;
    const bool debug = false;
    std::shared_ptr<output::Task> outputTask;
    std::shared_ptr<page_layout::Task> pageLayoutTask;
    std::shared_ptr<select_content::Task> selectContentTask;
    std::shared_ptr<deskew::Task> deskewTask;
    std::shared_ptr<page_split::Task> pageSplitTask;

    if (lastStage >= m_stages->outputFilterIdx()) {
      outputTask = m_stages->outputFilter()->createTask(page.id(), m_thumbnailCache, m_outFileNameGen, batch, debug);
    }
    if (lastStage >= m_stages->pageLayoutFilterIdx()) {
      pageLayoutTask = m_stages->pageLayoutFilter()->createTask(page.id(), outputTask, batch, debug);
    }
    if (lastStage >= m_stages->selectContentFilterIdx()) {
      selectContentTask = m_stages->selectContentFilter()->createTask(page.id(), pageLayoutTask, batch, debug);
    }
    if (lastStage >= m_stages->deskewFilterIdx()) {
      deskewTask = m_stages->deskewFilter()->createTask(page.id(), selectContentTask, batch, debug);
    }
    if (lastStage >= m_stages->pageSplitFilterIdx()) {
      pageSplitTask = m_stages->pageSplitFilter()->createTask(page, deskewTask, batch, debug);
    }
    auto fixOrientationTask = m_stages->fixOrientationFilter()->createTask(page.id(), pageSplitTask, batch);
    return std::make_shared<LoadFileTask>(BackgroundTask::BATCH, page, m_thumbnailCache, m_pages, fixOrientationTask);
  }

  int m_jobs;
  bool m_quiet;
  std::unique_ptr<ProjectReader> m_reader;
  std::shared_ptr<ProjectPages> m_pages;
  std::shared_ptr<StageSequence> m_stages;
  OutputFileNameGenerator m_outFileNameGen;
  std::shared_ptr<ThumbnailPixmapCache> m_thumbnailCache;
};

}  // namespace

int main(int argc, char* argv[]) {
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
  }
  Application app(argc, argv);

  // As the GUI does, so the same settings and default profile are used.
  Application::setApplicationName(APPLICATION_NAME);
  Application::setOrganizationName(ORGANIZATION_NAME);
  QSettings::setDefaultFormat(QSettings::IniFormat);
  if (app.isPortableVersion()) {
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, app.getPortableConfigPath());
  }
  {
    std::unique_ptr<ColorScheme> scheme
        = ColorSchemeFactory().create(ApplicationSettings::getInstance().getColorScheme());
    ColorSchemeManager::instance().setColorScheme(*scheme);
  }
  IconProvider::getInstance().setIconPack(StyledIconPack::createDefault());

  QString through = "page-layout";
  int jobs = static_cast<int>(std::thread::hardware_concurrency());
  bool quiet = false;
  QStringList positional;
  const QStringList args = Application::arguments();
  for (int i = 1; i < args.size(); ++i) {
    const QString& a = args[i];
    if (a == "-h" || a == "--help") {
      usage(stdout);
      return 0;
    } else if (a == "-q") {
      quiet = true;
    } else if ((a == "--through" || a == "--jobs") && i + 1 < args.size()) {
      if (a == "--through") {
        through = args[++i];
      } else {
        bool ok = false;
        jobs = args[++i].toInt(&ok);
        if (!ok || jobs < 1) {
          usage(stderr);
          return 2;
        }
      }
    } else if (a.startsWith("-")) {
      usage(stderr);
      return 2;
    } else {
      positional << a;
    }
  }
  if (positional.size() < 1 || positional.size() > 2) {
    usage(stderr);
    return 2;
  }
  int lastStage = -1;
  for (int i = 0; i < kStageCount; ++i) {
    if (through == kStageNames[i]) {
      lastStage = i;
    }
  }
  if (lastStage < 0) {
    std::fprintf(stderr, "scantailor-batch: unknown stage '%s'\n", through.toLocal8Bit().constData());
    usage(stderr);
    return 2;
  }

  try {
    const QString project = positional[0];
    const QString output = positional.size() == 2 ? positional[1] : project;
    Batch batch(project, jobs, quiet);
    const int failures = batch.process(lastStage);
    batch.save(output);
    if (!quiet) {
      std::fprintf(stderr, "Wrote %s\n", QDir::toNativeSeparators(output).toLocal8Bit().constData());
    }
    return failures ? 1 : 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "scantailor-batch: %s\n", e.what());
    return 1;
  }
}
