# Tradução: CMake → MSBuild

Regra transversal: propriedade vai em `<PropertyGroup>`, metadado de compilação vai em
`<ItemDefinitionGroup><ClCompile>`, e todo valor de item termina com `%(Metadado)` para não
apagar o que veio do `Directory.Build.props`. Ver `estrutura.md`.

## Alvos

| CMake | MSBuild | Onde |
|---|---|---|
| `add_executable(t …)` | `<ConfigurationType>Application</ConfigurationType>` (+ `<SubSystem>Console` ou `Windows` em `<Link>`) | PropertyGroup `Label="Configuration"` |
| `add_library(t STATIC …)` | `<ConfigurationType>StaticLibrary</ConfigurationType>` | idem |
| `add_library(t SHARED …)` | `<ConfigurationType>DynamicLibrary</ConfigurationType>` (+ `__declspec(dllexport)` ou `.def`; não existe `-fvisibility`) | idem |
| `add_library(t OBJECT …)` | **não existe** — vire `StaticLibrary` | — |
| `add_library(t INTERFACE)` | **não existe** — vire um `.props` que os consumidores importam | — |
| `add_custom_target(t)` | `<ConfigurationType>Utility</ConfigurationType>`, ou um `<Target>` dentro do projeto | — |
| `set_target_properties(t PROPERTIES OUTPUT_NAME x)` | `<TargetName>x</TargetName>` | PropertyGroup |
| `CMAKE_RUNTIME_OUTPUT_DIRECTORY` / `ARCHIVE_…` | `<OutDir>` / `<IntDir>` | PropertyGroup |
| `CMAKE_DEBUG_POSTFIX "_d"` | `<TargetName>$(ProjectName)_d</TargetName>` num PropertyGroup com `Condition` de Debug | PropertyGroup |

## Dependências entre alvos — e a armadilha das *usage requirements*

| CMake | MSBuild |
|---|---|
| `target_link_libraries(t PRIVATE outro_alvo)` | `<ProjectReference Include="..\outro\outro.vcxproj" />` |
| `target_link_libraries(t PRIVATE /caminho/foo.lib)` | `<Link><AdditionalDependencies>foo.lib;%(AdditionalDependencies)` + `<AdditionalLibraryDirectories>` |
| `target_include_directories(t PRIVATE dir)` | `<ClCompile><AdditionalIncludeDirectories>dir;%(AdditionalIncludeDirectories)` |
| `target_compile_definitions(t PRIVATE D=1)` | `<ClCompile><PreprocessorDefinitions>D=1;%(PreprocessorDefinitions)` |
| `target_compile_options(t PRIVATE /algo)` | `<ClCompile><AdditionalOptions>/algo %(AdditionalOptions)` — escotilha para o que não tem tag |
| `target_compile_features(t PRIVATE cxx_std_23)` / `CXX_STANDARD` | `<ClCompile><LanguageStandard>stdcpp23` |

⚠ **`PUBLIC`/`INTERFACE` não têm equivalente.** No CMake um alvo propaga includes e defines para
quem o consome. `<ProjectReference>` propaga **só o link do `.lib`** — include dirs, defines e
flags **não** são herdados. Duas saídas honestas:

- repetir includes/defines no consumidor (aceitável para duas ou três libs), ou
- escrever um `foo.props` ao lado do `foo.vcxproj` com as *usage requirements* e importar nos
  consumidores (`<Import Project="..\foo\foo.props" />`). É o mais próximo de
  `target_link_libraries(PUBLIC)` que o MSBuild oferece.

## Configurações e condicionais

| CMake | MSBuild |
|---|---|
| `CMAKE_BUILD_TYPE` | **não existe** — MSBuild é sempre multi-config; use `-p:Configuration=Debug` |
| `$<CONFIG:Debug>` | `Condition="'$(Configuration)'=='Debug'"` no `<PropertyGroup>`/`<ItemDefinitionGroup>` |
| `$<TARGET_FILE:t>` | `$(TargetPath)` dentro do próprio projeto; entre projetos, caminho montado com `$(OutDir)` |
| `$<BUILD_INTERFACE:…>` / `$<INSTALL_INTERFACE:…>` | não se aplica (não há `install()`) |
| `option(X "…" ON)` | `<X Condition="'$(X)'==''">true</X>`; sobrescreve com `-p:X=false`. Não há UI equivalente ao `cmake-gui` |
| `if(WIN32)` / `if(MSVC)` | some — o `.vcxproj` já é o ramo Windows |

## Geração de arquivos e passos customizados

| CMake | MSBuild |
|---|---|
| `add_custom_command(OUTPUT …)` | `<CustomBuild>` com `<Command>`, `<Outputs>`, `<AdditionalInputs>` — tem dependência incremental |
| `add_custom_command(TARGET t POST_BUILD …)` | `<PostBuildEvent><Command>` (idem `PreBuildEvent`, `PreLinkEvent`) |
| `configure_file(in.h.in out.h)` | **não existe.** Ou escreve o header à mão, ou um `<Target>` com a task `WriteLinesToFile`, ou um `<CustomBuild>` chamando um script de substituição |
| `file(GLOB …)` | o `.vcxproj` não tem laço — gere a lista (ver `estrutura.md`, seção 4) |

`<CustomBuild>` é o `add_custom_command` do MSBuild. Exemplo com compilação de shader:

```xml
<ItemGroup>
  <CustomBuild Include="shaders\bc1_encode.comp">
    <Command>"$(VULKAN_SDK)\Bin\glslangValidator.exe" -V "%(FullPath)" --vn bc1_encode_spv -o "$(IntDir)generated\bc1_encode_spv.hpp"</Command>
    <Outputs>$(IntDir)generated\bc1_encode_spv.hpp</Outputs>
    <Message>Compilando shader BC1</Message>
  </CustomBuild>
</ItemGroup>
```

…e `$(IntDir)generated` entra em `AdditionalIncludeDirectories`.

## O que simplesmente não vem junto

| CMake | Situação |
|---|---|
| `install()`, `CPack` | Sem equivalente. Substitua por um `<Target AfterTargets="Build">` com a task `Copy`, ou um script de publicação separado |
| `enable_testing()`, `add_test`, `ctest` | Sem equivalente nativo em C++. Ou um projeto de teste com adaptador do VS, ou mantenha o CTest só no build de Linux |
| `find_package(X)` | Sem equivalente. Resolve-se por vcpkg, por variável de ambiente (`$(VULKAN_SDK)`) ou por caminho num `.props` — ver `dependencias.md` |
| `pkg_check_modules` | Não existe no Windows. Cada dependência vira caminho explícito |
| `FetchContent` / `ExternalProject` | Sem equivalente. Ou submódulo git com `.vcxproj` próprio, ou vcpkg, ou binário pré-construído |
| `CMAKE_POSITION_INDEPENDENT_CODE`, `-fPIC` | Não se aplica no Windows |

## Flags do compilador

| CMake / GCC / Clang | MSBuild / MSVC |
|---|---|
| `-Wall -Wformat` | `<WarningLevel>Level4</WarningLevel>` (`/W4`) |
| `-Werror` | `<TreatWarningAsError>true</TreatWarningAsError>` |
| `CMAKE_CXX_STANDARD 20` / `23` | `<LanguageStandard>stdcpp20</LanguageStandard>` / `stdcpp23` |
| `-g` | `<DebugInformationFormat>ProgramDatabase</DebugInformationFormat>` |
| `-O0` | `<Optimization>Disabled</Optimization>` |
| `-O2` / `-O3` | `<Optimization>MaxSpeed</Optimization>` (`/O2` — o MSVC não tem `/O3`) |
| `-Os` | `<Optimization>MinSpace</Optimization>` |
| `-fno-omit-frame-pointer` | `<OmitFramePointers>false</OmitFramePointers>` |
| `-march=native -mtune=native` | `<EnableEnhancedInstructionSet>` com escolha **fixa** (`AdvancedVectorExtensions2`, …); não existe `native` |
| `-flto` / `-flto=thin` | `<WholeProgramOptimization>true</WholeProgramOptimization>` + `<LinkTimeCodeGeneration>UseLinkTimeCodeGeneration</LinkTimeCodeGeneration>` |
| `-ffunction-sections -Wl,--gc-sections` | `/Gy` + `<OptimizeReferences>true</OptimizeReferences>` |
| `-fsanitize=address` | `<EnableASAN>true</EnableASAN>` — é **`PropertyGroup`**, não metadado |
| `-fsanitize=undefined` | sem equivalente no MSVC até onde esta skill sabe — **confirme na sua versão** antes de afirmar; se não houver, é perda real de cobertura e vale dizer isso ao usuário |
| `-rdynamic` | não se aplica (símbolos vêm do `.pdb`) |
| `-fvisibility=hidden` | não se aplica; exportação é `__declspec(dllexport)` ou `.def` |
| `-pthread` | não se aplica |
| `-D FOO=1` | `<PreprocessorDefinitions>FOO=1;%(PreprocessorDefinitions)` |
| `-I dir` | `<AdditionalIncludeDirectories>dir;%(AdditionalIncludeDirectories)` |
| `-include x.h` | `<ForcedIncludeFiles>x.h;%(ForcedIncludeFiles)` |
| `/MD` vs `/MT` (runtime) | `<RuntimeLibrary>MultiThreadedDLL</RuntimeLibrary>` etc. — **tem que ser igual em todo objeto, `.lib` e DLL do processo** (skill `vs2026-msvc`) |

Duas notas que valem para qualquer máquina:

- **Opção desconhecida do `cl` é *warning*, não erro** (`D9002`): o build passa e a flag some.
  Leia os `D9002` do log antes de acreditar que a flag pegou.
- `stdcpp23` sai como `/std:c++23preview` nos MSVC 14.5x — é o esperado, não é erro.

## PCH

`target_precompile_headers(t pch.hpp)` vira `/Yu` em todo mundo e `/Yc` em **um** `.cpp`:

```xml
<ItemDefinitionGroup>
  <ClCompile>
    <PrecompiledHeader>Use</PrecompiledHeader>
    <PrecompiledHeaderFile>pch.hpp</PrecompiledHeaderFile>
  </ClCompile>
</ItemDefinitionGroup>
<ItemGroup>
  <ClCompile Include="src\pch\pch.cpp">
    <PrecompiledHeader>Create</PrecompiledHeader>
  </ClCompile>
</ItemGroup>
```

## Onde as flags globais moram

Tudo que era `CMAKE_CXX_FLAGS` global vira o `Directory.Build.props` da raiz: ele é importado
automaticamente por todo `.vcxproj` abaixo e é o análogo direto do `CMakeLists.txt` raiz. Ver a
seção 2 de `estrutura.md`.
