cmake_minimum_required(VERSION 3.20)
project(libwebp_subproject C)
set(CMAKE_UNITY_BUILD OFF)

# 1. Forçar que todas as bibliotecas internas sejam geradas como estáticas (.a)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static libraries" FORCE)

# 2. Configurações de otimização fornecidas por você (desativar utilitários de CLI)
set(WEBP_BUILD_ANIM_UTILS OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_CWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_DWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_GIF2WEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_IMG2WEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_VWEBP OFF CACHE BOOL "" FORCE) # OFF: vwebp is a GLUT/OpenGL viewer exe the app never uses (avoids the freeglut dep)
set(WEBP_BUILD_WEBPINFO OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPMUX ON CACHE BOOL "" FORCE)
set(WEBP_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)

# 3. Definir onde o código-fonte da libwebp realmente reside
# O seu script mencionou ${CMAKE_SOURCE_DIR}/external/libwebp
set(WEBP_SOURCE_DIR "${CMAKE_SOURCE_DIR}/external/libwebp")

if(NOT EXISTS "${WEBP_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Código-fonte da libwebp não encontrado em: ${WEBP_SOURCE_DIR}")
endif()

# 4. Adicionar o diretório oficial da libwebp. 
# Como está fora da árvore atual de subdiretórios, precisamos passar o caminho de build binário explicitamente.
add_subdirectory("${WEBP_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/libwebp_build")

# 5. Criar o ALIAS (WebP::webp) esperado pelo seu projeto principal
# O CMake oficial da libwebp gera um alvo interno chamado "webp"
if(TARGET webp)
    add_library(WebP::webp ALIAS webp)
    
    # Exportar a variável de inclusão que o seu root CMakeLists.txt usa nas linhas 44 e 75
    set(WEBP_INCLUDE_DIR "${WEBP_SOURCE_DIR}/src" CACHE INTERNAL "WebP Include Directory" FORCE)
    
    # Se houver alguma função de tratamento de perfil do seu projeto (como mencionado no seu trecho)
    if(COMMAND app_profilable_treatment)
        app_profilable_treatment(webp)
    endif()
endif()