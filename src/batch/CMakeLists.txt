# scantailor-batch: the processing stages without the GUI (see main.cpp).
set(CMAKE_AUTORCC ON)

set(batch_resource_files
    resources.qrc
    dark_scheme/dark_scheme.qrc
    light_scheme/light_scheme.qrc)
list_items_prepend(batch_resource_files "${SCANTAILOR_RESOURCES_DIR}/")

add_executable(scantailor-batch main.cpp ${batch_resource_files})
target_link_libraries(scantailor-batch PRIVATE core ${EXTRA_LIBS})
target_include_directories(
    scantailor-batch
    PRIVATE "$<TARGET_PROPERTY:TIFF::TIFF,INTERFACE_INCLUDE_DIRECTORIES>")
install(TARGETS scantailor-batch RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
