---
name: cmake-to-msbuild
description: Migrar (ou espelhar) um projeto CMake para uma solução nativa do Visual Studio — .slnx/.sln + .vcxproj + Directory.Build.props, com o build do Windows sem CMake nenhum. Cobre a decisão entre "gerar e cortar o cordão" e reescrever à mão, os esqueletos dos três arquivos, a tradução das construções do CMake para MSBuild (alvos, custom commands, install, generator expressions, flags do compilador), as dependências (vcpkg, deps de meson/autotools, Qt sem qt_add_qml_module) e como provar que a flag chegou no cl.exe. Use quando a tarefa mencionar migrar/converter CMake para Visual Studio, sair do CMake, solução nativa, native solution, .slnx, .sln, vcxproj, vcxproj.filters, Directory.Build.props, Directory.Build.targets, MSBuild, ProjectConfiguration, ItemDefinitionGroup, ClCompile, CustomBuild, ProjectReference, ZERO_CHECK, ALL_BUILD, cmake -G "Visual Studio", dotnet sln migrate, MSB4126, ou "quero abrir no VS sem CMake".
---

# Do CMake para uma solução nativa do Visual Studio

Existem só **duas rotas**, e a que todo mundo tenta primeiro não funciona.

## 1. "Gerar com o CMake e cortar o cordão" — não

`cmake -G "Visual Studio …"` não produz uma solução nativa: produz uma **casca que chama o
CMake de volta**. Meça na sua própria árvore gerada antes de apostar nela:

```powershell
cd <build-dir>
(Get-ChildItem *.vcxproj).Count
Select-String -Path *.vcxproj -Pattern 'cmake\.exe' -List | Measure-Object
Select-String -Path *.vcxproj -Pattern '<ConfigurationType>([^<]+)' -AllMatches |
  ForEach-Object { $_.Matches.Groups[1].Value } | Group-Object   # divida por nº de configurações
```

Exemplo real, de um app Qt/QML de **3 fontes**: 22 `.vcxproj`, **22 de 22** citando `cmake.exe`,
**22 de 22** com caminho absoluto embutido, **1** `Application` e **21** `Utility`
(`ZERO_CHECK`, `ALL_BUILD`, `INSTALL`, `*_autogen`, `*_qmltyperegistration`, `*_qmllint*`,
`*_copy_qml`, …) — e dois caminhos de `cmake.exe` diferentes gravados dentro, um deles restante
de uma configuração anterior. Quase tudo ali é andaime do CMake, não descrição de build; e o
caminho absoluto significa que o projeto é preso à máquina que o gerou.

Use a árvore gerada como **fonte de consulta** (quais fontes, quais defines e includes o CMake
resolveu) — nunca como o arquivo que você vai versionar.

## 2. Reescrever à mão — a rota real

Alvo por alvo, do menor para o maior. Esta skill é **autocontida**: os esqueletos dos três
arquivos estão em `references/estrutura.md`, a tradução de cada construção do CMake em
`references/traducao.md`.

Para o CMake em si (cache, geradores, presets), a skill `cmake`. Para `cl.exe`, toolsets,
`/std`, `/MD` vs `/MT` e ler `LNK*`/`MSB*`, a skill `vs2026-msvc`. Para entrar no dev shell e
achar o MSBuild, a skill `windows-cli`.

## As regras que não se quebram

1. **Projeto gerado pelo CMake não vira projeto nativo.** Ver a medição acima.
2. **`.slnx` (e `.sln`) substitui a solução, não o projeto.** Não existe C++ no Visual Studio
   descrito só por `.slnx`; ou há `.vcxproj`, ou é Open Folder + CMake — exatamente o que você
   está saindo.
3. **Flag no grupo errado é ignorada em silêncio.** `LanguageStandard`, `WarningLevel`,
   `PreprocessorDefinitions`, `AdditionalIncludeDirectories` são **metadados de `ClCompile`**
   (`<ItemDefinitionGroup>`); `PlatformToolset`, `EnableASAN`, `OutDir` são **propriedades**
   (`<PropertyGroup>`). Nada avisa quando você erra. Prove com
   `MSBuild … -v:diag | Select-String "CL.exe /c"`.
4. **Ordem dos `Import` no `.vcxproj`:** `Microsoft.Cpp.Default.props` → propriedades →
   `Microsoft.Cpp.props` → itens → `Microsoft.Cpp.targets`. Fora de ordem o projeto "compila"
   ignorando metade do que você configurou.
5. **Declare `<Configurations>` no `.slnx`.** Omitir dá `MSB4126: configuração de solução
   "Debug|x64" inválida` — e a mensagem não aponta para o `.slnx`.
6. **`%(Metadado)` no fim de todo valor de item.** Sem ele você apaga em silêncio o que o
   `Directory.Build.props` tinha posto. É o "append" do CMake.
7. **Decida o destino das construções sem equivalente ANTES de começar**, não no meio:
   `qt_add_qml_module`, `FetchContent`/`ExternalProject`, generator expressions,
   `configure_file`, `install()`, subprojetos meson/autotools.
8. **Um alvo por vez, linkando antes do próximo.** Comece pelo menor. Vinte `.vcxproj` escritos
   antes do primeiro link é vinte vezes o mesmo erro.
9. **Não apague o `CMakeLists.txt`.** Ele continua sendo o build de Linux/CI. Os dois convivem;
   só não espere que o CMake gere os `.vcxproj`.

## Classificar o problema

| Situação | Onde ir |
|---|---|
| "Dá pra aproveitar o que o CMake gerou?" / estimar o custo | `references/avaliacao.md` |
| Levantar fontes, defines e includes do projeto atual | `references/avaliacao.md` |
| Escrever o `.slnx`, o `.vcxproj` ou o `Directory.Build.props` | `references/estrutura.md` |
| `MSB4126`, ordem dos `Import`, GUID, `.filters`, build pela linha de comando | `references/estrutura.md` |
| `add_custom_command`, `configure_file`, `install()`, `option()`, `$<...>` | `references/traducao.md` |
| Flags do compilador (`-Wall`, `-O3`, `-flto`, sanitizers, PCH) | `references/traducao.md` |
| Debug/Release/RelWithDebInfo, `OutDir`, `TargetName` | `references/traducao.md` |
| vcpkg, dependência de meson/autotools, Qt sem CMake | `references/dependencias.md` |
| `MSB8020`, `LNK2038`, `/std` ignorado, toolset errado | skill `vs2026-msvc` |
| A flag não chegou no `cl.exe` | regra 3 acima, depois `vs2026-msvc` |

## Diagnóstico de abertura

Antes de opinar sobre a migração, meça o tamanho real do alvo:

```powershell
(Get-ChildItem src -Recurse -Include *.cpp,*.cxx,*.cc).Count
Select-String -Path CMakeLists.txt -Recurse -Pattern '^\s*(add_library|add_executable)' | Measure-Object
Select-String -Path CMakeLists.txt -Recurse -AllMatches -Pattern 'FetchContent|ExternalProject|qt_add_qml_module|configure_file|install\(|\$<|pkg_check_modules'
```

A terceira linha é a que decide o custo: cada acerto é uma construção **sem** equivalente
direto no MSBuild, e é onde a migração trava. Contar fontes só mede trabalho mecânico, que é
scriptável.

## Referências

| Arquivo | Conteúdo |
|---|---|
| `references/avaliacao.md` | Auditar a árvore gerada, inventariar o `CMakeLists.txt`, decidir a rota e a ordem de trabalho |
| `references/estrutura.md` | Esqueletos de `.slnx`, `.vcxproj` e `Directory.Build.props`; ordem dos `Import`; gerar a lista de fontes; build e verificação |
| `references/traducao.md` | Construções do CMake → MSBuild: alvos, links, custom commands, `install()`, `$<>`, multi-config, e a tabela de flags do compilador |
| `references/dependencias.md` | vcpkg com MSBuild, uma `StaticLibrary` por dep de fonte, deps de meson/autotools como binário, Qt sem `qt_add_qml_module` |

Neste repositório, `docs/windows-msvc-context.md` traz o ambiente Windows/MSVC medido e é bom
companheiro desta skill — com uma ressalva que importa: a §4 dele conclui que "o port manual é
desnecessário". Não há contradição, as duas respostas são para perguntas diferentes. Aquela
seção responde *"quero abrir a solução no VS"*, e para isso o `.slnx` que o próprio CMake gera
basta. Esta skill responde *"quero o build do Windows sem CMake nenhum"* — e é para essa que a
árvore gerada não serve, pelo motivo medido na abertura. Os números de versão, toolset e
caminhos daquele documento descrevem a máquina em que foi escrito; confira antes de repetir.

## Postura ao responder

- Responda no idioma do usuário.
- **Meça antes de estimar.** "É simples" e "é inviável" são as duas respostas erradas por
  padrão; o diagnóstico de abertura separa as duas em um minuto.
- Toda afirmação sobre "essa flag está ativa" precisa vir da linha de comando real do
  `cl.exe` (`-v:diag`), não do XML.
- Quando a construção não tem equivalente (`qt_add_qml_module`, `install()`, `$<>`), diga isso
  na cara e ofereça a substituição concreta — não invente uma tag de MSBuild.
- Nada nesta skill é específico de uma máquina. Se precisar de versão de toolset, caminho de
  SDK ou lista de toolsets instalados, **meça no ambiente atual** (skills `vs2026-msvc` e
  `windows-cli`) em vez de assumir.
