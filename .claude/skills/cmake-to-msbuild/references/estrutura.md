# Os três arquivos que substituem o CMakeLists

```
MinhaSolucao.slnx        <- lista de projetos (ou MinhaSolucao.sln, formato antigo)
Directory.Build.props    <- flags/defines/includes compartilhados: o "CMakeLists raiz"
projA/projA.vcxproj      <- fontes, custom builds, links
projB/projB.vcxproj
```

`.slnx` **não** substitui o `.vcxproj`. Não existe modelo de projeto C++ no Visual Studio em que
a solução sozinha descreva fontes, flags e links; o único modo "sem `.vcxproj`" que o VS oferece
para C++ é Open Folder + CMake — exatamente o que você está saindo.

## 1. A solução

```xml
<Solution>
  <Configurations>
    <Platform Name="x64" />
    <BuildType Name="Debug" />
    <BuildType Name="Release" />
  </Configurations>
  <Folder Name="/apps/">
    <Project Path="apps/meuapp/meuapp.vcxproj" />
  </Folder>
  <Folder Name="/libs/">
    <Project Path="thirdparty/imgui/imgui.vcxproj" />
  </Folder>
</Solution>
```

- Sem `<?xml …?>`, sem `xmlns`, sem `GlobalSection`. **GUID é opcional** — se você abrir um
  `.slnx` gerado por ferramenta vai ver `Type=` e `Id=` por projeto; ambas as formas são válidas.
- `<Folder Name="/apps/">` — barras no começo **e** no fim; são pastas virtuais.
- `<Project Path="…" />` é relativo ao arquivo de solução; barra normal funciona.
- **Omitir `<Configurations>` dá `MSB4126: configuração de solução "Debug|x64" inválida`.**
  Declare só as configurações que você vai manter — não precisa copiar as quatro do CMake.
- Dependência entre projetos vai no `.vcxproj` (`<ProjectReference>`), não aqui.

`.slnx` é o formato novo; versões mais antigas do Visual Studio e do MSBuild só abrem `.sln`.
Se a sua não abrir, use `.sln` — o resto desta skill não muda. Para converter um `.sln` legado,
`dotnet sln X.sln migrate`. Atenção: `dotnet sln add` **não aceita `.vcxproj`** (precisa do
MSBuild completo para avaliar o projeto); adicione pela IDE ou editando o XML à mão.

## 2. `Directory.Build.props` — o CMakeLists raiz

Na raiz do repositório. O MSBuild importa automaticamente em **todo** `.vcxproj` abaixo dele. É
o que evita repetir 30 include dirs e 6 defines em cada projeto.

⚠ **A armadilha nº 1:** quase tudo que parece "flag do compilador" **não é `<PropertyGroup>` —
é metadado do item `ClCompile`**, e precisa ir num `<ItemDefinitionGroup>`. No lugar errado, o
MSBuild avalia a propriedade em silêncio e ela **nunca chega no `cl.exe`**.

| Vai em `<PropertyGroup>` | Vai em `<ItemDefinitionGroup><ClCompile>` |
|---|---|
| `PlatformToolset`, `VCToolsVersion`, `WindowsTargetPlatformVersion` | `LanguageStandard`, `ConformanceMode` |
| `ConfigurationType`, `TargetName`, `OutDir`, `IntDir` | `WarningLevel`, `MultiProcessorCompilation`, `Optimization` |
| `EnableASAN`, `WholeProgramOptimization` | `PreprocessorDefinitions`, `AdditionalIncludeDirectories`, `AdditionalOptions` |

```xml
<Project>
  <PropertyGroup>
    <PlatformToolset>v143</PlatformToolset>
    <WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion>
    <OutDir>$(SolutionDir)build\$(Configuration)\</OutDir>
    <IntDir>$(SolutionDir)build\obj\$(MSBuildProjectName)\$(Configuration)\</IntDir>
  </PropertyGroup>

  <ItemDefinitionGroup>
    <ClCompile>
      <LanguageStandard>stdcpp20</LanguageStandard>
      <MultiProcessorCompilation>true</MultiProcessorCompilation>
      <WarningLevel>Level4</WarningLevel>
      <ConformanceMode>true</ConformanceMode>
      <AdditionalIncludeDirectories>$(SolutionDir)src;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
      <PreprocessorDefinitions>MEU_DEFINE;%(PreprocessorDefinitions)</PreprocessorDefinitions>
    </ClCompile>
  </ItemDefinitionGroup>

  <ItemDefinitionGroup Condition="'$(Configuration)'=='Debug'">
    <ClCompile>
      <PreprocessorDefinitions>_DEBUG;%(PreprocessorDefinitions)</PreprocessorDefinitions>
    </ClCompile>
  </ItemDefinitionGroup>
</Project>
```

O `PlatformToolset` acima é exemplo. **Escolha o toolset medindo o ambiente** (skill
`vs2026-msvc`), e fixe-o quando precisar casar com o toolset de uma dependência pré-construída
— Qt e afins, ver `dependencias.md`.

`%(X)` preserva o que já estava; sem ele você apaga o que veio de cima.

Um `Directory.Build.targets` no mesmo lugar roda **depois** dos projetos — é onde vão passos
globais de pós-build.

## 3. `.vcxproj` mínimo

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
    <RootNamespace>MeuApp</RootNamespace>
  </PropertyGroup>

  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.Default.props" />
  <PropertyGroup Label="Configuration">
    <ConfigurationType>Application</ConfigurationType>   <!-- ou StaticLibrary / DynamicLibrary -->
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.props" />

  <ItemGroup>
    <ClCompile Include="src\main.cpp" />
    <ClInclude Include="src\app.h" />
  </ItemGroup>

  <ItemGroup>
    <ProjectReference Include="..\..\thirdparty\imgui\imgui.vcxproj" />
  </ItemGroup>

  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.targets" />
</Project>
```

**A ordem dos `Import` é obrigatória:** `Microsoft.Cpp.Default.props` → propriedades de
configuração → `Microsoft.Cpp.props` → itens → `Microsoft.Cpp.targets`. Fora de ordem o projeto
"compila" ignorando metade do que você configurou, sem erro.

GUID novo: `[guid]::NewGuid().ToString().ToUpper()` no PowerShell.

**As configurações precisam existir em três lugares** — `<ProjectConfiguration>` aqui,
`<BuildType>`/`<Platform>` na solução, e a linha de comando. Faltando na solução: `MSB4126`.

## 4. Popular a lista de fontes

O `.vcxproj` não tem laço nem `file(GLOB)`. Gere os `<ClCompile>` a partir do disco em vez de
digitar centenas de linhas:

```powershell
Get-ChildItem src -Recurse -Include *.cpp |
  ForEach-Object { '    <ClCompile Include="' + (Resolve-Path $_.FullName -Relative) + '" />' }
```

O MSBuild aceita curinga no `Include` (`src\**\*.cpp`), o que resolveria o problema de vez —
mas a IDE tende a reescrever o item **expandido** quando você adiciona ou remove arquivo pelo
Solution Explorer. Se for usar, confirme com um build *e* com um salvamento pela IDE antes de
depender disso.

A árvore de pastas no Solution Explorer vem de um `MeuApp.vcxproj.filters` separado. É opcional:
sem ele tudo aparece achatado, e o build é idêntico.

## 5. Construir e verificar

```powershell
MSBuild MinhaSolucao.slnx -p:Configuration=Debug -p:Platform=x64 -m
```

Achar o `MSBuild.exe` e entrar num shell que compila é assunto da skill `windows-cli`
(`vswhere`, `VsDevCmd`, `Enter-VsDevShell`).

O ciclo de migração se faz na linha de comando, não na IDE: o erro é legível e o log é
inspecionável. A IDE entra no fim, para conferir o resultado.

**O único teste que prova que uma flag chegou:**

```powershell
MSBuild MinhaSolucao.slnx -t:Rebuild -p:Configuration=Debug -p:Platform=x64 -v:diag |
  Select-String "CL.exe /c" | Select-Object -First 1
```

`-v:diag` mostra a linha de comando real do `cl.exe`. Para investigação mais funda, `-bl` grava
um `msbuild.binlog` com tudo. Ler o resultado (por que `/std` foi ignorado, de onde veio um
include, o que o linker procurou) é a skill `vs2026-msvc`.
