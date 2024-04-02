#設定最低版本要求
cmake_minimum_required(VERSION 2.8)

#設定project名稱
project(WEB_SERVER)

#增加編譯選項
add_compile_options(-Wall -g -std=c++11)

#設定產生的可執行檔保存的路徑
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin)

#1、讀取目錄下的所有原始碼，存入變數SRC中
aux_source_directory(${PROJECT_SOURCE_DIR}/src SRC)

#2、將原始碼對應的頭檔包含到路徑中
include_directories(${PROJECT_SOURCE_DIR}/inc)

#3、產生可執行文件
add_executable(main main.cpp ${SRC})

#4、連接動態庫
target_link_libraries(main pthread)
