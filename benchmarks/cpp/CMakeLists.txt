add_executable(bench_pipeline bench_pipeline.cpp)
target_link_libraries(bench_pipeline PRIVATE handwriting_core)

if(MSVC)
    target_compile_options(bench_pipeline PRIVATE /W4 /utf-8)
else()
    target_compile_options(bench_pipeline PRIVATE -Wall -Wextra -Wpedantic)
endif()
