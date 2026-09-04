# Dependências sem CMake

No CMake as dependências são um grafo que a ferramenta resolve. No MSBuild **você** resolve, à
mão, uma por uma. Classifique cada dependência antes de escrever qualquer XML — a caixa
determina o trabalho, e a terceira é a que mata cronograma.

| Caixa | Como entra na solução | Custo |
|---|---|---|
| **Fonte** (ImGui, stb, single-header libs, libs pequenas em C/C++) | um `.vcxproj` `StaticLibrary` por dependência + `<ProjectReference>` | baixo, mecânico |
| **Pacote** (curl, libwebp, zlib, taglib…) | vcpkg, que integra nativamente com MSBuild | baixo |
| **Build alienígena** (meson, autotools, `configure`) | **binário pré-construído**, nunca dentro da solução | alto ou proibitivo |

## Caixa 1 — dependência de fonte

Um `.vcxproj` com `<ConfigurationType>StaticLibrary</ConfigurationType>`, as fontes da
biblioteca, e nada mais. O consumidor a puxa com `<ProjectReference>`.

Lembre que `<ProjectReference>` **não propaga include dirs nem defines** (ver `traducao.md`):
ou repete no consumidor, ou escreve um `foo.props` ao lado do `foo.vcxproj` com as *usage
requirements* e importa nos consumidores.

Todas as libs da solução têm que concordar na **biblioteca de runtime** (`/MD` vs `/MT`) e na
configuração. Divergiu, é `LNK2038` no melhor caso e corrupção de heap silenciosa no pior —
assunto da skill `vs2026-msvc`.

## Caixa 2 — vcpkg

O vcpkg integra com MSBuild sem passar por CMake. Confira se a integração global está ativa:

```powershell
Test-Path "$env:LOCALAPPDATA\vcpkg\vcpkg.user.props"
Get-Content "$env:LOCALAPPDATA\vcpkg\vcpkg.user.props"
```

Esse arquivo é o que `vcpkg integrate install` escreve, e ele importa o `vcpkg.props` da sua
instalação. Com ele presente, **todo** `.vcxproj` da máquina recebe os includes e libs do vcpkg
sem nenhuma configuração no projeto. Consequências práticas:

- instale no triplet certo: `x64-windows` (DLL, casa com `/MD`) ou `x64-windows-static` (casa
  com `/MT`). Triplet trocado dá `LNK2038` ou DLL faltando em runtime;
- a integração é **global por usuário**, o que é conveniente e também significa que um projeto
  pode estar pegando includes que você não declarou. Ao depurar include misterioso, lembre-se;
- modo manifesto (`vcpkg.json` ao lado do projeto) funciona com MSBuild via
  `<VcpkgEnableManifest>true</VcpkgEnableManifest>` no `.vcxproj`. Confirme com um build limpo
  antes de depender disso num CI.

## Caixa 3 — meson, autotools, configure

Não tente trazer para dentro da solução. O MSBuild não vai orquestrar um `meson setup` de forma
sã, e o resultado é um projeto que ninguém consegue reconstruir.

A rota é construir a dependência **fora**, uma vez, e consumir o resultado como binário:
`<AdditionalDependencies>` + `<AdditionalLibraryDirectories>`, com a raiz numa propriedade
(`$(FooRoot)`) definida no `Directory.Build.props` ou vinda de variável de ambiente. As DLLs vão
para o lado do executável por um `<Target AfterTargets="Build">` com a task `Copy`.

⚠ **ABI antes de tudo:** binário oficial de Windows construído com **MinGW não linka com o
MSVC**. É comum em toda a família multimídia (mpv, libplacebo, FFmpeg — este último exige
`configure --toolchain=msvc` hospedado em MSYS2). Se a dependência só existe em mingw, as opções
honestas são: construir do fonte com o toolchain MSVC, trocar a dependência, ou **cortar a
funcionalidade no primeiro corte** e reintroduzir depois. Diga qual das três, não prometa as
quatro.

Dependências que existem só no Linux (EGL de sistema, fontconfig, `pkg-config`) não têm
substituto direto: ou o código ganha um caminho alternativo (DirectWrite no lugar de fontconfig,
por exemplo), ou a funcionalidade fica de fora do build do Windows.

## SDKs por variável de ambiente

Vulkan, CUDA e afins não passam por `find_package`: use a variável que o instalador define, já
visível no MSBuild como propriedade — `$(VULKAN_SDK)\Include`,
`"$(VULKAN_SDK)\Bin\glslangValidator.exe"`. Um erro explícito no `Directory.Build.props`
transforma "não achou o header" numa mensagem que se entende:

```xml
<Target Name="CheckVulkan" BeforeTargets="ClCompile">
  <Error Condition="'$(VULKAN_SDK)'==''" Text="VULKAN_SDK não está definido — instale o SDK do LunarG." />
</Target>
```

## Qt sem CMake

O ponto duro, e não tem contorno bonito: **`qt_add_qml_module` é API exclusiva do CMake.** É ela
que gera o `qmldir`, o `qmltyperegistrations.cpp` e a marcação de singleton. O Qt VS Tools não
tem equivalente — e é por isso que a árvore gerada tem os alvos `*_autogen`,
`*_qmltyperegistration` e `*_qmllint*` (ver `avaliacao.md`).

Duas rotas:

- **Abandonar o módulo QML formal** — recomendada quando são poucas classes expostas. Troque os
  `QML_ELEMENT` por `qmlRegisterType<MinhaClasse>("MeuModulo", 1, 0, "MinhaClasse")` no
  `main.cpp`, os singletons por `qmlRegisterSingletonType(QUrl("qrc:/qml/Theme.qml"), …)`, e
  empacote os `.qml` num `.qrc` simples carregado por `qrc:/`. Elimina `qmldir`,
  `qmltyperegistrar` e a dependência da extensão — sobra só o `moc`.
- **Preservar o módulo**: `qmldir` escrito à mão (incluindo as linhas `singleton`), `.qrc` com
  os `.qml` + `qmldir`, `qmltyperegistrar` como `<CustomBuild>` para os `QML_ELEMENT`, e
  opcionalmente `qmlcachegen`. As três ferramentas ficam em `<QtDir>\bin\`.

Mais dois pontos de atenção:

- **Toolset.** O nome da pasta da instalação do Qt diz com que toolset ele foi construído
  (`msvc2019_64` → v142, `msvc2022_64` → v143). Fixe `<PlatformToolset>` para casar com os
  `.lib` do Qt em vez de apostar em compatibilidade entre toolsets; se precisar, fixe também
  `<VCToolsVersion>`. Qual toolset existe na máquina é medição, não suposição — skill
  `vs2026-msvc`.
- **Qt VS Tools** declara no manifesto uma faixa de versões do Visual Studio. Se a sua for mais
  nova que a faixa, confirme que a extensão realmente carrega **antes** de contar com ela para
  `moc`/`rcc`/`uic`. Se não carregar, `moc` roda como `<CustomBuild>` à mão, com
  `<Outputs>moc_x.cpp</Outputs>` e o resultado incluído na compilação.
