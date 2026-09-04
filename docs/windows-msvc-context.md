# Contexto Windows/MSVC deste repositório

> **Medido em 03/09/2026 nesta máquina**, compilando e *executando* binários para ler
> `_MSC_FULL_VER` — nunca deduzindo do nome do toolset. Números envelhecem; cada seção traz
> o comando que reconfere. **Se a máquina discordar deste arquivo, a máquina está certa.**
>
> Substitui o antigo `docs/vs2026-slnx-port.md` (port manual `.slnx`/MSBuild), que partia de
> duas premissas hoje falsas: que só havia uma instalação do VS, e que o CMake não servia
> para o alvo Qt. Ver §1 e §4. Aquele arquivo não está mais neste branch; se precisar
> consultá-lo, ele sobrevive no branch `qtwinmsv-broken-backup`
> (`git show qtwinmsv-broken-backup:docs/vs2026-slnx-port.md`).

## 0. O resumo em cinco linhas

O trabalho de Windows/MSVC vive no branch **`qt-windows-msvc`** (`git switch
qt-windows-msvc`). O `main` carrega este documento para referência; mudanças de
Windows pertencem ao branch. Índice geral em [`README.md`](README.md).

| | |
|---|---|
| Toolset do repositório | **`v145`** — a geração **v180**, MSVC **14.51.36231**, `_MSC_FULL_VER 195136256` |
| Gerador | `Visual Studio 18 2026`, x64, resolvido para a instalação **Community** |
| Alvo que compila no Windows | `QT/VulkanMedia` → `appVulkanMedia.exe`, via preset `vs2026` |
| Alvo que **não** compila no Windows | `example_sdl3_vulkan` (app principal) — ver §6 |
| Solução gerada | `build/vs2026/example_sdl3_vulkan.slnx` — o CMake **já emite `.slnx`** |

## 1. Há **duas** instalações do VS 2026, e elas não são iguais

Esta é a armadilha nº 1 da máquina, e não existia quando o guia anterior foi escrito:

| | Community | Enterprise |
|---|---|---|
| Caminho | `...\Microsoft Visual Studio\18\Community` | `...\Microsoft Visual Studio\18\Enterprise` |
| `installationVersion` | `18.9.12120.119` | `18.10.12120.281` |
| `catalog_productDisplayVersion` | `18.9.2` | **`Insiders [12120.281]`** |
| Toolsets em `VC\v180\` | `ClangCL`, `v145` | `ClangCL`, `v145` |
| Toolsets em `VC\v170\` | **`v143`** | **vazio** |
| Pastas em `VC\Tools\MSVC` | 14.29.30133, 14.42.34433, 14.44.35207, 14.50.35717, 14.51.36231, 14.52.36615 | 14.44.35207, 14.50.35717, 14.51.36231, 14.52.36629 |

**As duas ferramentas escolhem instalações diferentes:**

- `vswhere -latest` → **Enterprise** (é a de maior versão);
- o gerador do CMake → **Community**, gravado no cache como
  `CMAKE_GENERATOR_INSTANCE:INTERNAL=C:/Program Files/Microsoft Visual Studio/18/Community`.

Ou seja: o `cl.exe` do seu Developer Shell **não é** necessariamente o `cl.exe` que compilou
`build/vs2026`. Antes de comparar builds, confira qual instalação respondeu.

### 1.1 Medição: o que cada instalação aceita

Compilado e executado com um `.vcxproj` mínimo, um `printf("%d", _MSC_FULL_VER)`:

| Instalação | `-p:PlatformToolset=` | Resultado | `_MSC_FULL_VER` | MSVC |
|---|---|---|---|---|
| Community | `v145` | ✅ compila e roda | `195136256` | 14.51.36231 |
| Community | `v143` | ✅ compila e roda | `194435228` | 14.44.35207 |
| Enterprise | `v145` | ✅ compila e roda | `195136256` | 14.51.36231 |
| Enterprise | `v143` | ❌ **`MSB8020`** | — | — |

O `MSB8020` do Enterprise tem causa concreta e verificável sem compilar:
`MSBuild\Microsoft\VC\v170\Platforms\x64\PlatformToolsets\` está **vazio** nessa instalação.
`v141`/`v142` falham nas duas pelo mesmo motivo (`v150\` e `v160\` vazios).

> **Consequência prática:** `v145` é o único toolset presente nas **duas** instalações.
> Qualquer coisa fixada em `v143` compila só no Community — e quebra no dia em que o
> Developer Shell abrir no Enterprise.

## 2. `v180` é a pasta; `v145` é o alias

`v180` **não é** um valor válido para `<PlatformToolset>`. É o nome da pasta de *targets* do
MSBuild — `MSBuild\Microsoft\VC\v180\` — onde moram os toolsets daquela geração:

```
VC\v150\  (vazio)          <- seria o v141
VC\v160\  (vazio)          <- seria o v142
VC\v170\  -> v143          <- só no Community
VC\v180\  -> v145, ClangCL <- nas duas
```

Escrever `<PlatformToolset>v180</PlatformToolset>` dá **`MSB8020`** — medido, nas duas
instalações, com o mesmo `.vcxproj` mínimo da §1.1. **O alias da geração v180 é `v145`** — e
é ele que este repositório usa.

### 2.1 Os quatro números da mesma coisa

Citar "a versão do MSVC" sem dizer qual das numerações não significa nada:

| Onde | Nomeado por | Valor (v145) | Valor (v143) |
|---|---|---|---|
| `<PlatformToolset>` | alias do toolset | `v145` | `v143` |
| Pasta em `VC\Tools\MSVC\` | versão da pasta | `14.51.36231` | `14.44.35207` |
| `_MSC_FULL_VER` no binário | versão do compilador | `195136256` | `194435228` |
| `_MSC_VER` | idem, truncado | `1951` | `1944` |

O `_MSC_FULL_VER` do v145 (`…36256`) **não bate** com o nome da pasta (`…36231`), e isso é
normal — são espaços de numeração distintos, não erro de transcrição.

Pegadinha herdada: `Microsoft.VCToolsVersion.v143.default.txt` hoje declara `14.44.35207`
(coincide com o build real), mas já declarou `14.42.34433` enquanto o MSBuild usava 14.44.
Se a versão exata importar, **fixe `VCToolsVersion` explicitamente** em vez de confiar no
arquivo `.default.txt`.

## 3. Como o repositório é configurado hoje (verificado)

O preset `vs2026` do `CMakePresets.json`:

```jsonc
{
  "name": "vs2026",
  "generator": "Visual Studio 18 2026",
  "binaryDir": "${sourceDir}/build/vs2026",
  "architecture": { "value": "x64" },
  "cacheVariables": {
    "BUILD_VULKAN_MEDIA_ONLY": "ON",      // pula o app principal, Linux-only (§6)
    "CMAKE_PREFIX_PATH": "$env{QTDIR}",   // = C:\Qt\6.11.2\msvc2022_64
    "CMAKE_CONFIGURATION_TYPES": "Debug;Release;RelWithDebInfo"
  }
}
```

O `.vcxproj` que o CMake **gera** (`build/vs2026/QT/VulkanMedia/appVulkanMedia.vcxproj`)
sai com:

| Propriedade | Valor gerado |
|---|---|
| `<PlatformToolset>` | **`v145`** |
| `<LanguageStandard>` | `stdcpp20` (o `CMakeLists.txt` do VulkanMedia pede C++20) |
| `<RuntimeLibrary>` | `MultiThreadedDebugDLL` / `MultiThreadedDLL` (`/MDd`, `/MD`) |
| `<WindowsTargetPlatformVersion>` | `10.0.28000.0` |

Nenhum toolset está fixado no preset — o `v145` acima é o **padrão** que o gerador escolheu.
Para tornar isso explícito (e imune à instalação que for selecionada), acrescente ao preset:

```jsonc
"toolset": "v145"
```

⚠ Isso exige **reconfigurar do zero** (`cmake --preset vs2026 --fresh`). O CMake recusa mudar
o toolset de um cache existente, com esta mensagem exata (medida):

```
CMake Error: Error: generator toolset: v145
Does not match the toolset used previously:
```

(o campo depois de `previously:` sai vazio justamente porque o cache atual não fixou toolset.)

### 3.1 Comandos

```powershell
$env:QTDIR = "C:\Qt\6.11.2\msvc2022_64"     # já está setado nesta máquina
cmake --preset vs2026                        # configure
cmake --build build/vs2026 --config Debug --target appVulkanMedia
.\build\vs2026\QT\VulkanMedia\Debug\appVulkanMedia.exe
```

O `CMakeUserPresets.json` (gerado pelo Qt Creator) traz os presets `Qt-Debug`/`Qt-Release`,
que usam **Ninja**, não o VS — são outro caminho de build, não o desta seção.

## 4. O CMake já gera `.slnx` — o port manual é desnecessário

O guia anterior partia de que "solução nativa sem CMake" exigia escrever `.slnx` e `.vcxproj`
à mão. **Não exige.** O CMake 4.4.3 com o gerador `Visual Studio 18 2026` emite
`build/vs2026/example_sdl3_vulkan.slnx` — formato novo, com `<Configurations>`, `Type` e `Id`
por projeto e `<BuildDependency>` entre eles:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<Solution>
  <Configurations>
    <BuildType Name="Debug"/><BuildType Name="Release"/><BuildType Name="RelWithDebInfo"/>
    <Platform Name="x64"/>
  </Configurations>
  <Project Path="QT/VulkanMedia/appVulkanMedia.vcxproj" Type="8bc9ceb8-..." Id="8d6f13c9-...">
    <BuildDependency Project="ZERO_CHECK.vcxproj"/>
    ...
  </Project>
</Solution>
```

Abra esse arquivo no VS (`File > Open > Project/Solution`) e você tem a solução nativa, com
`moc`/`rcc`/`qmltyperegistrar` já ligados pelo `qt_add_qml_module` — sem depender do Qt VS
Tools e sem reescrever o módulo QML à mão.

`build/` é ignorado pelo git (`.gitignore:2`), então a solução gerada não entra no repositório
— ela é reproduzível por `cmake --preset vs2026`.

## 5. Qt `msvc2022_64` linkado com `v145`

O kit instalado é `C:\Qt\6.11.2\msvc2022_64` — o nome diz **v143 / MSVC 14.4x**. O build
deste repositório usa **v145 / 14.51**. Isso funciona: `cmake --build build/vs2026 --config
Debug --target appVulkanMedia` compila e linka
`build/vs2026/QT/VulkanMedia/Debug/appVulkanMedia.exe` (verificado em 03/09/2026), pela
compatibilidade binária do MSVC dentro da faixa 14.x.

Duas condições que **não** podem ser violadas:

1. **Biblioteca de runtime igual em tudo.** O Qt `msvc2022_64` é `/MD`; o `.vcxproj` gerado
   também. Misturar `/MT` aqui dá `LNK2038` — ou corrupção de heap silenciosa.
2. **`.pch` e `.ifc` não atravessam toolsets.** PCH e módulos C++ são ABI do toolset que os
   gerou; ao trocar de toolset, rebuild completo, não incremental.

Os outros kits Qt disponíveis (`llvm-mingw_64`, `mingw_64`, `msvc2022_arm64`, wasm, android)
**não** são intercambiáveis com o MSVC x64 — o mingw usa libstdc++ e outra ABI.

## 6. O app principal continua Linux-only

`example_sdl3_vulkan` (o alvo do `CMakeLists.txt` raiz) não configura no Windows, e é por isso
que o preset `vs2026` liga `BUILD_VULKAN_MEDIA_ONLY=ON`. Os bloqueios reais:

- **Defaults de Linux no CMakeLists raiz** — `VCPKG_TARGET_TRIPLET` cai em `x64-linux` e
  `VCPKG_ROOT` em `$HOME/vcpkg`. No Windows seria
  `-DVCPKG_TARGET_TRIPLET=x64-windows -DVCPKG_ROOT=C:/vcpkg`.
- **pkg-config obrigatório** — `CMakeLists.txt:98` faz `find_package(PkgConfig REQUIRED)` e as
  linhas 99–108 pedem `freetype2`, `egl`, `gl` e `fontconfig` via `pkg_check_modules(...
  REQUIRED)`. Não há pkg-config nem EGL de sistema no Windows, e o `REQUIRED` derruba o
  configure ali mesmo.
- **Preset `all`** — fixa `clang`/`clang++` + `CMAKE_LINKER_TYPE=LLD`, não MSVC.
- **Vulkan SDK ausente** — `$env:VULKAN_SDK` vazio e `C:\VulkanSDK` não existe. Sem ele não
  há `glslangValidator` para os shaders, nem headers/loader do Vulkan. Instale o LunarG SDK
  antes de tentar.
- **As três dependências de mídia não são CMake.** `thirdparty/libplacebo/CMakeLists.txt:22` e
  `thirdparty/mpv/CMakeLists.txt:29` invocam `meson setup` por `ExternalProject`;
  `thirdparty/ffmpeg/CMakeLists.txt:37` chama o `configure` autotools de `external/FFmpeg`.
  No Windows isso quer dizer meson + um shell POSIX (MSYS2) e, no caso do FFmpeg,
  `--toolchain=msvc`. Ou binários pré-construídos, ou o caminho de vídeo fica de fora do
  primeiro corte.

`vcpkg` está em `C:\vcpkg` e integrado globalmente ao MSBuild. As dependências do
`vcpkg.json` (curl, libwebp, reflectcpp, taglib — mais `cppwinrt` no Windows) instalam no
triplet `x64-windows` sem drama.

## 7. Windows SDK

Quatro instalados, em `Program Files (x86)\Windows Kits\10\Include`:
`10.0.19041.0`, `10.0.22621.0`, `10.0.26100.0`, `10.0.28000.0`.

O `10.0.10240.0` que existia em medições anteriores **não está mais aqui**. O padrão do
`.vcxproj` gerado é **`10.0.28000.0`**.

## 8. Reconferir tudo

```powershell
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"

# as instalações e o que cada uma oferece
& $vswhere -all -prerelease -products * -format value -property installationPath | ForEach-Object {
  "--- $_"
  Get-ChildItem "$_\MSBuild\Microsoft\VC" -Directory -ErrorAction SilentlyContinue | ForEach-Object {
    "   $($_.Name): " + ((Get-ChildItem "$($_.FullName)\Platforms\x64\PlatformToolsets" `
        -Directory -ErrorAction SilentlyContinue).Name -join ', ')
  }
}

# qual instalação o CMake escolheu para este build tree
Select-String -Path build\vs2026\CMakeCache.txt -Pattern 'GENERATOR_INSTANCE|CMAKE_LINKER:'

# qual toolset o .vcxproj gerado pediu
Select-String -Path build\vs2026\QT\VulkanMedia\appVulkanMedia.vcxproj `
  -Pattern 'PlatformToolset|LanguageStandard|RuntimeLibrary|WindowsTargetPlatformVersion' |
  Sort-Object Line -Unique

# Windows SDKs e Vulkan
Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\Include" -Directory | Select-Object -ExpandProperty Name
$env:VULKAN_SDK
```

E, para provar qual compilador de fato rodou — o único teste que vale — compile um
`printf("%d", _MSC_FULL_VER)` e **execute** o binário. Caminho em log não prova nada: o log
traz includes e libs de várias versões ao mesmo tempo.
