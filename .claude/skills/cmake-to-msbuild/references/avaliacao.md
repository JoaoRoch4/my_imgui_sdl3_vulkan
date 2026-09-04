# Avaliar a migração: o que dá pra aproveitar, quanto custa, por onde começar

## 1. Auditar a árvore que o CMake gerou

Se já existe um diretório configurado com o gerador do Visual Studio, ele responde de graça
duas perguntas: **quanto o CMake gera por baixo do pano** e **que fontes e flags ele resolveu**.

```powershell
cd <build-dir>
(Get-ChildItem *.vcxproj).Count
Select-String -Path *.vcxproj -Pattern 'cmake\.exe' -List | Measure-Object
Select-String -Path *.vcxproj -Pattern '[A-Za-z]:[\\/]' -List | Measure-Object
Select-String -Path *.vcxproj -Pattern '<ConfigurationType>([^<]+)' -AllMatches |
  ForEach-Object { $_.Matches.Groups[1].Value } | Group-Object
Select-String -Path *.vcxproj -Pattern '[A-Za-z]:[\\/][^"]*cmake\.exe' -AllMatches |
  ForEach-Object { $_.Matches.Value } | Sort-Object -Unique
```

O `Group-Object` conta uma ocorrência **por configuração**: divida pelo número de configurações
para saber quantos projetos são de cada tipo.

Ordem de grandeza a esperar, medida num app Qt/QML de 3 fontes: 22 `.vcxproj`, todos citando
`cmake.exe`, todos com caminho absoluto, **1** `Application` contra **21** `Utility`, e dois
`cmake.exe` diferentes gravados dentro — um deles sobra de uma configuração anterior, apontando
para um CMake que nem é o do cache atual. Projeto que carrega caminho absoluto de ferramenta é,
por definição, preso à máquina e ao momento em que foi gerado.

Se o seu número for parecido, a rota "gerar e cortar o cordão" está respondida.

**O que vale a pena extrair da árvore gerada** (valores, não estrutura):

```powershell
# as fontes que o CMake realmente compilou
Select-String -Path <alvo>.vcxproj -Pattern '<ClCompile Include="([^"]+)"' -AllMatches |
  ForEach-Object { $_.Matches.Groups[1].Value }
# defines e includes já resolvidos, com os caminhos de Qt/vcpkg/SDK expandidos
Select-String -Path <alvo>.vcxproj -Pattern 'PreprocessorDefinitions|AdditionalIncludeDirectories'
```

### `.slnx` ou `.sln`?

Depende da versão do CMake e do gerador — versões recentes já emitem `.slnx`. Não presuma:
`Get-ChildItem *.sln, *.slnx` no diretório de build responde. Duas diferenças entre um arquivo
gerado e o esqueleto escrito à mão, ambas legítimas (não conclua que uma delas está errada):

- o gerado costuma trazer `Type=` e `Id=` por projeto; **GUID em `.slnx` é opcional**;
- o gerado declara as quatro configurações do CMake (`Debug`, `Release`, `MinSizeRel`,
  `RelWithDebInfo`) e usa `<BuildDependency>`; à mão você declara só as que vai manter e põe a
  dependência no `.vcxproj` (`<ProjectReference>`).

### Se precisar rodar o configure para produzir essa árvore

Rode de um Developer PowerShell / Developer Command Prompt. Fora do ambiente do Visual Studio o
configure pode falhar com `The CXX compiler identification is unknown` /
`No CMAKE_CXX_COMPILER could be found`, ou pior, escolher outro compilador — assunto da skill
`cmake`, não desta.

## 2. Inventariar o `CMakeLists.txt`

O custo da migração **não** é proporcional ao número de fontes: listar fontes é mecânico e
scriptável. O custo está nas construções sem equivalente.

```powershell
Select-String -Path CMakeLists.txt -Recurse -AllMatches -Pattern 'FetchContent|ExternalProject|qt_add_qml_module|configure_file|install\(|add_custom_|\$<|pkg_check_modules|add_subdirectory' |
  Group-Object Filename
```

Classifique cada acerto numa de três caixas, **por escrito, antes de abrir o editor**:

| Caixa | O que significa | Exemplos |
|---|---|---|
| **Traduz** | existe tag de MSBuild equivalente | `target_link_libraries`, `target_include_directories`, `add_custom_command`, `CXX_STANDARD` |
| **Substitui** | não traduz, mas há outra forma de obter o resultado | `configure_file` → header escrito à mão; `qt_add_qml_module` → `qmlRegisterType` + `.qrc`; `FetchContent` → vcpkg ou submódulo com `.vcxproj` próprio |
| **Corta** | não vai existir no build do Windows | `install()`/CPack, `pkg_check_modules`, alvos Linux-only, sanitizers que o MSVC não tem |

Se a caixa **Substitui** passar de meia dúzia de itens, a migração é um projeto, não uma tarefa
— diga isso ao usuário antes de começar, não no meio.

## 3. Sinais de que a rota "gerar e cortar" está descartada de saída

- **O `CMakeLists.txt` não configura no Windows**: `pkg_check_modules`, flags GCC/Clang cruas
  (`-Wall`, `-rdynamic`, `-march=native`, `-flto=thin`), `add_subdirectory` de dependências com
  build meson/autotools. O configure morre antes de gerar `.vcxproj` — não há o que aproveitar.
- **Qt com `qt_add_qml_module`**: os alvos gerados (`*_autogen`, `*_qmltyperegistration`,
  `*_qmllint*`) **são** o CMake trabalhando; sem ele, não existem.
- **Dependências entram por `FetchContent`**: o grafo gerado inclui os projetos de terceiros
  inteiros, com os caminhos absolutos do cache.

## 4. Ordem de trabalho

1. O menor alvo que produz um binário — de preferência um `StaticLibrary` sem dependência
   externa. Compile e **link** antes de escrever o segundo.
2. `Directory.Build.props` na raiz assim que houver o *segundo* projeto, não antes; com um só,
   ele esconde onde a flag mora.
3. Os demais alvos, um a um, do menos dependente para o mais dependente.
4. O executável final por último.
5. Só então abra na IDE. O ciclo de migração é MSBuild na linha de comando, que dá erro legível.

Em cada passo, o teste de verdade é a linha do `cl.exe`, não o XML:

```powershell
MSBuild MinhaSolucao.slnx -t:Rebuild -p:Configuration=Debug -p:Platform=x64 -v:diag |
  Select-String "CL.exe /c" | Select-Object -First 1
```

Para um projeto grande, considere um **primeiro corte sem as funcionalidades que dependem das
dependências difíceis** (ver `dependencias.md`) só para ter o app abrindo no Windows, e
reintroduza a pilha pesada depois. É mais rápido chegar a um binário que roda e regredir a
partir dele do que perseguir paridade total antes do primeiro link.
