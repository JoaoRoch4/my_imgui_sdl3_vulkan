# Guia rápido — solução nativa VS 2026 (.slnx + MSBuild), sem CMake

> Tudo abaixo foi verificado nesta máquina em 2026-08-30 (versões, toolset e o
> esqueleto `.slnx` foram compilados de verdade com o MSBuild do VS 2026 antes de
> entrar neste documento).
>
> **Revisão do mesmo dia:** os componentes `v143` foram instalados depois da
> primeira redação, e as §1 e §1.1 foram refeitas em cima da máquina já com eles.
> A conclusão anterior de que "`v143` não funciona no VS 2026" **estava errada** —
> ver §1.1. Foram revisadas §1, §1.1 e §7a; as demais seções seguem como na
> primeira redação e não foram reverificadas.

## 0. Antes de tudo: o que `.slnx` é e o que não é

`.slnx` substitui o `.sln` — o arquivo **de solução**. Ele **não** substitui o
`.vcxproj`. Não existe modelo de projeto C++ no Visual Studio em que o `.slnx`
sozinho descreva fontes, flags e links.

Portanto "solução nativa em slnx sem CMake" na prática significa:

```
MinhaSolucao.slnx        <- lista de projetos (XML enxuto, novo formato)
Directory.Build.props    <- flags/defines/includes compartilhados (o "root CMakeLists")
projA/projA.vcxproj      <- MSBuild: fontes, custom builds, links
projB/projB.vcxproj
```

O único modo "sem `.vcxproj`" que o VS oferece para C++ é **Open Folder + CMake** —
exatamente o que você está saindo.

## 1. Ambiente detectado nesta máquina

| Item | Valor verificado |
|---|---|
| Visual Studio | Community **2026**, `18.9.12120.119`, `C:\Program Files\Microsoft Visual Studio\18\Community` |
| PlatformToolset | **`v145`** é o padrão (pasta de targets: `MSBuild\Microsoft\VC\v180\`) — o alias `v145` nunca é `v180`. **`v143` e `ClangCL` também estão disponíveis e compilam**; `v141`/`v142` não. Ver §1.1. |
| MSVC toolchain padrão | **14.51.36231** (pasta) / `14.51.36256` (pacote) — *não* 14.52.36615, que é o **Preview**. Seis toolchains lado a lado, ver §1.1. |
| MSBuild | `...\18\Community\MSBuild\Current\Bin\MSBuild.exe` |
| Qt | `C:\Qt\6.11.2\msvc2022_64` — o nome indica build com **v143** (MSVC 14.4x). Com o v143 agora instalado dá pra casar o toolset exatamente; ver §7a. |
| Qt VS Tools | 3.5.0 instalado — ⚠ o manifesto declara `[17.12, 18.0)`, ou seja **VS 2022**. Confirme que ele realmente carrega no VS 2026 antes de apostar nele. |
| vcpkg | `C:\vcpkg` |
| Vulkan SDK | **ausente** — instale o LunarG SDK se for portar o app principal |
| CMake | 4.4.3, gerador `Visual Studio 18 2026` disponível |

`.vs/my_imgui_sdl3_vulkan.slnx/v18/` já existe, mas é só **cache** do VS 18. Não há
nenhum `.slnx` / `.vcxproj` versionado no repositório ainda — você começa do zero.

### 1.1 Os três números de versão — e por que `v143` **funciona**

Isso confunde de verdade, e a raiz da confusão é que a mesma coisa tem **três**
numerações diferentes. Medidas nesta máquina, para o toolset padrão:

| Onde | Nomeado por | Valor |
|---|---|---|
| MSBuild `<PlatformToolset>` | alias do toolset | `v145` |
| VS Installer (`vswhere -include packages`) | versão do **pacote** MSVC | `Microsoft.VisualCpp.Tools.HostX64.TargetX64` → `14.51.36256` |
| Pasta em `VC\Tools\MSVC\` | versão da **pasta** | `14.51.36231` |
| `_MSC_FULL_VER` no código | versão do **compilador** | `195136256` (= 19.51.36256) |

O número do pacote (`…36256`) bate com o `_MSC_FULL_VER` e **nenhum dos dois bate
com o nome da pasta** (`…36231`). O mesmo vale no v143: pasta `14.44.35207`,
compilador `194435228`. Não é erro de transcrição — são espaços de numeração
distintos, e citar "a versão do MSVC" sem dizer qual dos três não quer dizer nada.

Buscar `v145` nos **Componentes individuais** do Installer dá vazio, e isso está
correto: nenhum pacote tem `v145` no id. No Installer você procura pela **versão
do MSVC** (`14.51`, `14.44`, …), não pelo alias. É a relação de sempre:
v143 ↔ MSVC 14.4x, v145 ↔ 14.5x.

#### Correção: `v143` compila no VS 2026

A primeira versão deste guia afirmava que `-p:PlatformToolset=v143` morria com
`MSB8020`. **Isso estava errado.** Os componentes `v143` aparecem marcados
*"(Sem Suporte)"* na busca do Installer, mas isso é um rótulo de **suporte**, não
de usabilidade — instalado o componente, o toolset compila e linka normalmente.

Verificado com um `.vcxproj` mínimo real, compilando e **executando** o binário
para ler o `_MSC_FULL_VER`:

| `-p:PlatformToolset=` | pasta de targets | MSVC usado | `_MSC_FULL_VER` | resultado |
|---|---|---|---|---|
| `v145` (padrão) | `VC\v180\` | 14.51.36231 | 195136256 | ✅ compila e roda |
| `v143` | `VC\v170\` | 14.44.35207 | 194435228 | ✅ compila e roda |
| `ClangCL` | `VC\v180\` | `VC\Tools\Llvm\x64\bin\clang-cl.exe` + `lld-link.exe` | 195136256 | ✅ compila e roda |
| `v142` | `VC\v160\` | — | — | ❌ `MSB8020` |
| `v141` | `VC\v150\` | — | — | ❌ `MSB8020` |

O `MSB8020` de `v141`/`v142` é real e tem causa concreta: as pastas `VC\v150\` e
`VC\v160\` existem, mas o `Platforms\x64\PlatformToolsets\` dentro delas está
**vazio**. Só `VC\v170\` (→ `v143`) e `VC\v180\` (→ `v145`, `ClangCL`) têm
toolset instalado. Para saber o que dá pra usar sem tentar compilar:

```powershell
$base = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Microsoft\VC"
Get-ChildItem $base -Directory | ForEach-Object {
  "$($_.Name): " + ((Get-ChildItem "$($_.FullName)\Platforms\x64\PlatformToolsets" `
      -Directory -ErrorAction SilentlyContinue).Name -join ', ')
}
```

Ferramentas de terceiros (CLion, Qt Creator) que listam kits só até v143 continuam
sem conhecer o v145 — é limitação do detector delas, não do MSBuild.

#### Os seis toolchains soltos em `VC\Tools\MSVC\` — e o perdido

São seis pastas, ~50 GB somadas, mas só **duas** são escolhidas sem você pedir:

| Pasta | GB | Componente no Installer | Como chegar nela |
|---|---|---|---|
| `14.29.30133` | 0,91 | **nenhum** | ❌ inalcançável — ver abaixo |
| `14.42.34433` | 9,43 | `…Component.VC.14.42.17.12.x86.x64` | `v143` + `-p:VCToolsVersion=14.42.34433` |
| `14.44.35207` | 9,48 | `…Component.VC.14.44.17.14.x86.x64` | **padrão do `v143`** |
| `14.50.35717` | 9,70 | `…Component.VC.14.50.18.0.x86.x64` | `v145` + `-p:VCToolsVersion=14.50.35717` |
| `14.51.36231` | 10,23 | `…Component.VC.14.51.x86.x64` | **padrão do `v145`** |
| `14.52.36615` | 10,23 | nenhum (Preview, vem com o VS) | `v145` + `-p:VCToolsVersion=14.52.36615` |

O `17.12` / `17.14` no id do componente é a versão do **VS 2022** à qual aquele
MSVC pertence — é por isso que os componentes `v143` vêm com aviso de "sem
suporte", e é essa a string que você procura no Installer.

**`14.29.30133` é o perdido de verdade.** Está no disco sem componente
correspondente (sobra de instalação anterior) e não há como usá-lo. Pedir
explicitamente responde:

```
error MSB8052: O Conjunto de Ferramentas do MSVC Versão '14.29.30133' não é
compatível com o Conjunto de Ferramentas da Plataforma 'v143'. Altere o Conjunto
de Ferramentas da Plataforma para v142 [...]
```

…e `v142` é exatamente o que dá `MSB8020`. Beco sem saída: usar essa pasta exigiria
instalar o componente *"MSVC v142 — VS 2019 C++ build tools"*. São 0,91 GB parados;
não dá pra removê-los pelo Installer, já que não há componente que os reivindique.

**Pegadinha de reprodutibilidade:** o arquivo
`VC\Auxiliary\Build\Microsoft.VCToolsVersion.v143.default.txt` declara
`14.42.34433`, mas o build real com `v143` saiu com **`14.44.35207`**. Se a versão
exata importa (ABI com binários de terceiros, Qt inclusive), **fixe o
`VCToolsVersion` explicitamente** no `Directory.Build.props` em vez de confiar no
padrão:

```xml
<PropertyGroup>
  <PlatformToolset>v143</PlatformToolset>
  <VCToolsVersion>14.44.35207</VCToolsVersion>
</PropertyGroup>
```

## 2. Não tente o atalho "gerar com CMake e cortar o cordão"

O reflexo natural é rodar `cmake -G "Visual Studio 18 2026"` uma vez, ficar com os
`.vcxproj` gerados e apagar o CMake. **Isso não configura neste repositório.** O
`CMakeLists.txt` raiz é Linux-only por construção:

- `find_package(PkgConfig REQUIRED)` + `pkg_check_modules(freetype2 / egl / gl / fontconfig)`
- flags GCC/Clang cruas: `-Wall`, `-rdynamic`, `-march=native`, `-flto=thin`, `-fsanitize=...`
- `add_subdirectory(thirdparty/...)` que envolvem builds meson/autotools

O configure quebra antes de gerar qualquer `.vcxproj`. O port é escrito à mão.

## 3. O esqueleto `.slnx` (validado — compila)

```xml
<Solution>
  <Configurations>
    <Platform Name="x64" />
    <BuildType Name="Debug" />
    <BuildType Name="Release" />
  </Configurations>
  <Folder Name="/apps/">
    <Project Path="QT/VulkanMedia/VulkanMedia.vcxproj" />
  </Folder>
  <Folder Name="/libs/">
    <Project Path="thirdparty/imgui/imgui.vcxproj" />
  </Folder>
</Solution>
```

Notas de sintaxe (todas confirmadas empiricamente):

- Sem `<?xml ...?>`, sem GUIDs, sem `xmlns`, sem blocos `GlobalSection`.
- `<Folder Name="/apps/">` — barras no começo **e** no fim; são pastas virtuais.
- `<Project Path="..."/>` — caminho relativo ao `.slnx`; barra normal funciona.
- **Se você omitir `<Configurations>`**, o MSBuild falha com
  `MSB4126: configuração de solução "Debug|x64" inválida`. Declare sempre.
- Dependências entre projetos vão no `.vcxproj` (`<ProjectReference>`), não no `.slnx`.

Build pela linha de comando:

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" `
    MinhaSolucao.slnx -p:Configuration=Debug -p:Platform=x64 -m
```

Se um dia você tiver um `.sln` legado, `dotnet sln X.sln migrate` gera o `.slnx`
(o .NET 10 já cria `.slnx` por padrão no `dotnet new sln`). Cuidado: `dotnet sln add`
**não aceita `.vcxproj`** (precisa do MSBuild completo para avaliar) — adicione via
IDE ou editando o XML à mão.

## 4. `Directory.Build.props` — o equivalente ao seu CMakeLists raiz

Coloque um na raiz do repositório. O MSBuild importa automaticamente em **todo**
`.vcxproj` abaixo dele. É isto que evita duplicar 30 include dirs e 6 defines em
cada projeto.

⚠ **A armadilha nº 1:** quase tudo que parece "flag do compilador" **não é uma
`<PropertyGroup>` — é metadado do item `ClCompile`**, e precisa ir num
`<ItemDefinitionGroup>`. Posto no lugar errado o MSBuild avalia a propriedade em
silêncio e ela **nunca chega no `cl.exe`**. `PlatformToolset` e `EnableASAN` são
propriedades de projeto; `LanguageStandard`, `WarningLevel`,
`MultiProcessorCompilation`, `PreprocessorDefinitions`,
`AdditionalIncludeDirectories` são metadados.

```xml
<Project>
  <PropertyGroup>
    <PlatformToolset>v145</PlatformToolset>
    <WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion>
    <OutDir>$(SolutionDir)build\$(Configuration)\</OutDir>
    <IntDir>$(SolutionDir)build\obj\$(MSBuildProjectName)\$(Configuration)\</IntDir>
  </PropertyGroup>

  <ItemDefinitionGroup>
    <ClCompile>
      <LanguageStandard>stdcpp23</LanguageStandard>
      <MultiProcessorCompilation>true</MultiProcessorCompilation>
      <WarningLevel>Level4</WarningLevel>            <!-- ~ -Wall -Wformat -->
      <ConformanceMode>true</ConformanceMode>        <!-- /permissive- -->
      <AdditionalIncludeDirectories>$(SolutionDir)code;$(SolutionDir)code\core;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
      <PreprocessorDefinitions>APP_USE_UNLIMITED_FRAME_RATE;IMGUI_ENABLE_FREETYPE_PLUTOSVG;%(PreprocessorDefinitions)</PreprocessorDefinitions>
    </ClCompile>
  </ItemDefinitionGroup>

  <ItemDefinitionGroup Condition="'$(Configuration)'=='Debug'">
    <ClCompile>
      <PreprocessorDefinitions>_DEBUG;APP_USE_VULKAN_DEBUG_REPORT;%(PreprocessorDefinitions)</PreprocessorDefinitions>
    </ClCompile>
  </ItemDefinitionGroup>
</Project>
```

Com esse arquivo, a linha de comando real do `cl.exe` (capturada de um build de
teste com este `.slnx`) sai assim — ou seja, tudo chegou:

```
CL.exe /c /W4 /MP /D APP_USE_UNLIMITED_FRAME_RATE /std:c++23preview /permissive- ... bar.cpp
```

Repare que `stdcpp23` vira `/std:c++23preview` no MSVC 14.5x — é o esperado.

⚠ **O `v145` acima é o padrão genérico.** Para a solução do `QT/VulkanMedia`,
troque por `v143` + `VCToolsVersion` fixo — o Qt instalado é `msvc2022_64`. Ver
§7a; o porquê está na §1.1.

`%(X)` preserva o que já estava — o análogo do "append" do CMake.
Um `Directory.Build.targets` (mesmo lugar) faz o mesmo *depois* dos projetos, útil
para custom builds globais.

**Como conferir que uma flag realmente chegou:**

```powershell
MSBuild MinhaSolucao.slnx -t:Rebuild -p:Configuration=Debug -p:Platform=x64 -v:diag |
  Select-String "CL.exe /c" | Select-Object -First 1
```

Este é o teste que separa "a propriedade foi avaliada" de "a flag foi passada".

## 5. Esqueleto mínimo de `.vcxproj` (validado — compila)

```xml
<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Debug|x64">
      <Configuration>Debug</Configuration><Platform>x64</Platform>
    </ProjectConfiguration>
    <ProjectConfiguration Include="Release|x64">
      <Configuration>Release</Configuration><Platform>x64</Platform>
    </ProjectConfiguration>
  </ItemGroup>

  <PropertyGroup Label="Globals">
    <ProjectGuid>{GERE-UM-GUID-NOVO}</ProjectGuid>
    <RootNamespace>VulkanMedia</RootNamespace>
  </PropertyGroup>

  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.Default.props" />
  <PropertyGroup Label="Configuration">
    <ConfigurationType>Application</ConfigurationType>   <!-- ou StaticLibrary -->
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.props" />

  <ItemGroup>
    <ClCompile Include="src\main.cpp" />
    <ClInclude Include="src\filesystemmodel.h" />
  </ItemGroup>

  <ItemGroup>
    <ProjectReference Include="..\..\thirdparty\imgui\imgui.vcxproj" />
  </ItemGroup>

  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.targets" />
</Project>
```

GUID novo: `[guid]::NewGuid().ToString().ToUpper()` no PowerShell.

Como o CMake lista fontes em blocos comentados e o `.vcxproj` não tem loop, gere os
`<ClCompile>` a partir do disco em vez de digitar 137 linhas:

```powershell
Get-ChildItem code -Recurse -Include *.cpp |
  ForEach-Object { '    <ClCompile Include="' + (Resolve-Path $_.FullName -Relative) + '" />' }
```

A árvore de pastas no Solution Explorer vem de um `NomeDoProjeto.vcxproj.filters`
separado (opcional; sem ele tudo aparece achatado).

## 6. Tradução das flags do projeto

| CMake / Clang atual | MSBuild / MSVC |
|---|---|
| `-Wall -Wformat` | `<WarningLevel>Level4</WarningLevel>` (`/W4`) |
| `CMAKE_CXX_STANDARD 23` | `<LanguageStandard>stdcpp23</LanguageStandard>` |
| `-g` (Debug/RelWithDebInfo) | `<DebugInformationFormat>ProgramDatabase</DebugInformationFormat>` |
| `-fno-omit-frame-pointer` | `<OmitFramePointers>false</OmitFramePointers>` |
| `-O3` | `<Optimization>MaxSpeed</Optimization>` (`/O2` — MSVC não tem `/O3`) |
| `-march=native -mtune=native` | `<EnableEnhancedInstructionSet>AdvancedVectorExtensions2</EnableEnhancedInstructionSet>` (escolha fixa; não existe `native`) |
| `-flto=thin` | `<WholeProgramOptimization>true</WholeProgramOptimization>` + `<LinkTimeCodeGeneration>UseLinkTimeCodeGeneration</LinkTimeCodeGeneration>` |
| `-ffunction-sections -Wl,--gc-sections` | `/Gy` + `<OptimizeReferences>true</OptimizeReferences>` |
| `-fsanitize=address` | `<EnableASAN>true</EnableASAN>` — **`PropertyGroup`**, verificado: gera `/fsanitize=address` |
| `-fsanitize=undefined` | **não existe no MSVC** — perda real de cobertura vs. o Debug atual |
| `-rdynamic` | não se aplica (Windows usa `.pdb` para símbolos) |
| `target_precompile_headers(pch.hpp)` | `/Yc` em **um** `pch.cpp` + `/Yu` no resto (ver abaixo) |
| `target_link_libraries(...)` | `<AdditionalDependencies>` ou `<ProjectReference>` |
| `CMAKE_DEBUG_POSTFIX "_d"` | `<TargetName>$(ProjectName)_d</TargetName>` num `PropertyGroup` com `Condition` de Debug |

PCH no `.vcxproj`:

```xml
<ItemDefinitionGroup>
  <ClCompile>
    <PrecompiledHeader>Use</PrecompiledHeader>
    <PrecompiledHeaderFile>pch.hpp</PrecompiledHeaderFile>
  </ClCompile>
</ItemDefinitionGroup>
<ItemGroup>
  <ClCompile Include="code\pch\pch.cpp">
    <PrecompiledHeader>Create</PrecompiledHeader>
  </ClCompile>
</ItemGroup>
```

O passo `glslangValidator -V bc1_encode.comp --vn bc1_encode_spv` vira um
`<CustomBuild>` (é o `add_custom_command` do MSBuild — inclusive com dependência
incremental):

```xml
<ItemGroup>
  <CustomBuild Include="code\rendering\vulkan\shaders\bc1_encode.comp">
    <Command>"$(VULKAN_SDK)\Bin\glslangValidator.exe" -V "%(FullPath)" --vn bc1_encode_spv -o "$(IntDir)generated\bc1_encode_spv.hpp"</Command>
    <Outputs>$(IntDir)generated\bc1_encode_spv.hpp</Outputs>
    <Message>Compilando shader BC1</Message>
  </CustomBuild>
</ItemGroup>
```

…e `$(IntDir)generated` entra em `AdditionalIncludeDirectories`.

## 7. Por onde começar — os dois alvos são muito diferentes

### 7a. `QT/VulkanMedia` — comece por aqui ✅

5 fontes + 19 QML. Qt 6.11.2 msvc já instalado, branch já é `qt-windows-msvc`.
Viável em algumas horas. Caminho: Qt VS Tools cuida de `moc`/`rcc`/`uic` dentro do
MSBuild — mas confirme antes que a extensão 3.5.0 carrega no VS 2026 (§1).

**Use `v143` aqui, não o `v145` padrão.** O Qt instalado é `msvc2022_64`, ou seja
compilado com o toolset v143 / MSVC 14.4x. Com o v143 agora disponível (§1.1) dá
pra casar o toolset com o dos `.lib` do Qt em vez de apostar em
compatibilidade entre toolsets — e é a mesma faixa que o manifesto do Qt VS Tools
declara (`[17.12, 18.0)`), o que remove a segunda incógnita de uma vez só. No
`Directory.Build.props` do §4, para esta solução:

```xml
<PropertyGroup>
  <PlatformToolset>v143</PlatformToolset>
  <VCToolsVersion>14.44.35207</VCToolsVersion>
</PropertyGroup>
```

(O `v145` não foi testado contra o Qt — o argumento aqui não é que ele quebra, é
que com o v143 instalado não há mais motivo para descobrir.)

**O ponto duro, e é real:** `qt_add_qml_module` é **API exclusiva do CMake**. Ele
gera hoje, para você, o `qmldir`, o `qmltyperegistrations.cpp` e a marcação
`QT_QML_SINGLETON_TYPE` do `Theme.qml`. O Qt VS Tools não tem equivalente.

**Rota recomendada — abandone o módulo QML formal.** São só 2 classes expostas
(`FileSystemModel`, `ThumbnailProvider`):

- troque os `QML_ELEMENT` por `qmlRegisterType<FileSystemModel>("VulkanMedia", 1, 0, "FileSystemModel")` em `main.cpp`;
- `Theme.qml` vira `qmlRegisterSingletonType(QUrl("qrc:/qml/Theme.qml"), ...)`;
- empacote os 19 `.qml` + `Format.js` num `.qrc` simples e carregue por `qrc:/`.

Isso elimina `qmldir`, `qmltyperegistrar` e a dependência de o Qt VS Tools carregar
no VS 2026 — sobra só `moc` (que você pode rodar como `<CustomBuild>` à mão se a
extensão não subir).

**Rota fiel ao módulo QML**, se quiser preservar a estrutura atual:

1. escrever o `qmldir` à mão (incluindo `singleton Theme 1.0 Theme.qml`);
2. empacotar os `.qml` + `qmldir` num `.qrc`;
3. rodar `qmltyperegistrar.exe` como `<CustomBuild>` para os `QML_ELEMENT`;
4. opcionalmente `qmlcachegen.exe` para pré-compilar.

As três ferramentas existem em `C:\Qt\6.11.2\msvc2022_64\bin\` (verificado).

### 7b. `example_sdl3_vulkan` (app principal) — o `.vcxproj` é fácil, as deps não

137 fontes: gerar o `.vcxproj` é um dia de trabalho. As dependências é que travam.

**Tranquilas no MSVC:** SDL3, Vulkan (instale o LunarG SDK), ImGui/ImPlot/stb (são
só fontes — compile como `StaticLibrary`), e as quatro do `vcpkg.json` (curl,
libwebp, reflectcpp, taglib) — o vcpkg integra nativamente com MSBuild
(`vcpkg integrate install`, triplet `x64-windows`).

**Bloqueadas ou caras:**

- **mpv** — build meson; os binários oficiais de Windows são mingw, não MSVC
- **libplacebo** — meson
- **FFmpeg** — precisa de `configure` hospedado em MSYS2 com `--toolchain=msvc`
- **EGL/GL** (`placebo_egl_context.cpp`) — não há EGL de sistema no Windows; o
  caminho zero-copy NVDEC→GL→Vulkan precisa ser reescrito ou desligado
- **fontconfig** — `FcFontMatch` em `load_fonts()` precisa de fallback via DirectWrite
- **plutovg / plutosvg** — compiláveis, mas mais um par de `.vcxproj` à mão

Ordem realista: um `.vcxproj` `StaticLibrary` por dependência que compila do fonte,
e as três de meson/autotools consumidas como binários pré-construídos
(`<AdditionalDependencies>` + `AdditionalLibraryDirectories`) — nunca como parte da
solução. Considere um primeiro corte sem vídeo (sem mpv/libplacebo/FFmpeg) só para
ter o app abrindo no Windows, e reintroduza a pilha de mídia depois.

## 8. Passo a passo mínimo

1. Escreva o `.slnx` (§3) e o `.vcxproj` (§5) **à mão**, num editor de texto. Os
   dois esqueletos deste guia já foram compilados juntos com sucesso — partir
   deles é mais rápido que reconciliar um template.
   *(Se preferir a IDE: `File > New > Project` → **Empty Project** funciona, mas o
   template emite configurações `Win32`; você terá que adicionar `x64` e remover
   `Win32` antes de qualquer coisa. E confirme o formato do arquivo salvo — o
   default `.slnx` está confirmado para o `dotnet new sln` do .NET 10, não
   necessariamente para a IDE.)*
2. Crie o `Directory.Build.props` na raiz (§4) — atenção ao `PropertyGroup` vs
   `ItemDefinitionGroup`.
3. Popule os `<ClCompile>` com o script do §5.
4. `MSBuild MinhaSolucao.slnx -p:Configuration=Debug -p:Platform=x64 -m` até linkar.
   Confira as flags com o `-v:diag | Select-String "CL.exe /c"` do §4.
5. Abra no VS: `File > Open > Project/Solution` → o `.slnx`.
6. Só então adicione o segundo projeto ao `.slnx`.
7. Mantenha o `CMakeLists.txt` — ele ainda é o build de Linux. Os dois convivem;
   apenas não espere que o CMake gere os `.vcxproj`.

Nota: o vcpkg desta máquina já está integrado globalmente ao MSBuild — o build de
teste saiu com `/I"C:\vcpkg\installed\x64-windows\include"` sem nenhuma
configuração. Só instale os pacotes no triplet `x64-windows`.

## 9. `.gitignore`

```
.vs/
build/
*.vcxproj.user
x64/
```
