# ImPlot, compiled once into a static `implot` lib. implot.h includes <imgui.h>,
# so the imgui dependency is propagated PUBLIC-ly to consumers.
set(IMPLOT_DIR ${CMAKE_SOURCE_DIR}/external/implot)

add_library(implot STATIC
    ${IMPLOT_DIR}/implot.cpp
    ${IMPLOT_DIR}/implot_items.cpp
)
target_include_directories(implot SYSTEM PUBLIC ${IMPLOT_DIR})
target_link_libraries(implot PUBLIC imgui)
target_compile_options(implot PRIVATE -w)
app_profilable_treatment(implot)
