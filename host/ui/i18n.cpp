// The interface's translations (ui/i18n.hpp). Each entry is the English text
// as the code writes it, then its translation. A pattern for trf() keeps its
// {} where the code puts a name or a number.

#include "ui/i18n.hpp"

#include "settings/settings.hpp"

#include <SDL3/SDL.h>

#include <string_view>
#include <unordered_map>

namespace portablekit::ui {
namespace {

struct Entry {
    const char *english;
    const char *portuguese;
};

// Brazilian Portuguese.
constexpr Entry kPortuguese[] = {
    // The menu's frame.
    {"Settings", "Configurações"},
    {"Paused", "Pausado"},
    {"Running", "Em execução"},
    {"Video", "Vídeo"},
    {"Audio", "Áudio"},
    {"Controls", "Controles"},
    {"Network", "Rede"},
    {"System", "Sistema"},
    {"Close", "Fechar"},
    {"Resume", "Continuar"},
    {"Select", "Selecionar"},
    {"Back", "Voltar"},
    {"Section", "Seção"},
    {"Change", "Alterar"},
    {"Cancel", "Cancelar"},
    {"Quit", "Sair"},
    {"Done", "Concluído"},
    {"Later", "Depois"},
    {"On", "Ligado"},
    {"Off", "Desligado"},
    {"Yes", "Sim"},
    {"No", "Não"},
    // The game's save list (save_chooser.cpp).
    {"Remastered movies", "Vídeos remasterizados"},
    {"Mods", "Mods"},
    {"Choose the folder in the window that opened.", "Escolha a pasta na janela que abriu."},
    {"Choose the file in the window that opened.", "Escolha o arquivo na janela que abriu."},
    {"Open the window again", "Abrir a janela de novo"},
    {"If the window is hidden or was closed.", "Se a janela ficou escondida ou foi fechada."},
    {"Browse here instead", "Procurar aqui mesmo"},
    {"Choose with the list in this window.", "Escolher pela lista nesta janela."},
    {"Choose nothing.", "Não escolher nada."},
    {"Movies", "Vídeos"},
    {"Skip movies with START", "Pular vídeos com START"},
    {"START during a movie ends it and goes on with the game.",
     "START durante um vídeo o encerra e segue com o jogo."},
    {"Open the movies folder", "Abrir a pasta de vídeos"},
    {"Where remastered movies go: NAME.264, for the game's movie NAME. Kit Movie Tools puts them there.",
     "Onde ficam os vídeos remasterizados: NOME.264, para o vídeo NOME do jogo. O Kit Movie Tools os coloca lá."},
    {"No remastered movies found", "Nenhum vídeo remasterizado encontrado"},
    {"Game files", "Arquivos do jogo"},
    {"Replaced files", "Arquivos substituídos"},
    {"Reads the files a mod puts in the mods/files folder of the game's data in place of the disc's files at the same path (mods/files/PSP_GAME/USRDIR/…): translations, models, sounds. Takes effect the next time the game loads them.",
     "Lê os arquivos que um mod coloca na pasta mods/files dos dados do jogo no lugar dos arquivos do disco no mesmo caminho (mods/files/PSP_GAME/USRDIR/…): traduções, modelos, sons. Vale a partir da próxima vez que o jogo os carregar."},
    {"No replaced files found", "Nenhum arquivo substituído encontrado"},
    {"Open the files folder", "Abrir a pasta de arquivos"},
    {"Where a mod's files go, in the disc's folders: PSP_GAME/USRDIR/…",
     "Onde ficam os arquivos de um mod, nas pastas do disco: PSP_GAME/USRDIR/…"},
    {"{} found", "{} encontrados"},
    {"Plays the remastered movies a mod puts in the mods/movies folder of the game's data (NAME.264, NAME the game's movie) in place of the game's own, with the game's sound.",
     "Toca os vídeos remasterizados que um mod coloca na pasta mods/movies dos dados do jogo (NOME.264, NOME o vídeo do jogo) no lugar dos originais, com o som do jogo."},
    {"Load", "Carregar"},
    {"Save", "Salvar"},
    {"Choose", "Escolher"},
    {"New save data", "Novo save"},
    {"Save over this save?", "Salvar por cima deste save?"},
    {"Delete this save?", "Apagar este save?"},
    {"Slot {}", "Slot {}"},
    {"Loading…", "Carregando…"},
    {"Saving…", "Salvando…"},
    {"Deleting…", "Apagando…"},
    {"Saved.", "Salvo."},
    {"Deleted.", "Apagado."},
    {"Could not save.", "Não foi possível salvar."},
    {"Could not delete.", "Não foi possível apagar."},
    {"1 save", "1 save"},
    {"{} saves", "{} saves"},
    {"None", "Nenhum"},
    {"Default", "Padrão"},
    {"Type", "Digitar"},
    {"Cursor", "Cursor"},
    {"Symbols", "Símbolos"},
    {"Shift", "Shift"},
    {"Space", "Espaço"},
    {"Delete", "Apagar"},
    {"Settings and pause", "Configurações e pausa"},

    // Video.
    {"Picture", "Imagem"},
    {"Resolution", "Resolução"},
    {"Auto", "Auto"},
    {"Auto draws the game at the window's own size and follows it (at most 1632 lines). ×1 to ×6 draw 272 lines "
     "per step, 480×272 times the step unless the aspect ratio is Fill. Higher is sharper and needs more from the GPU.",
     "Auto desenha o jogo no tamanho da própria janela e acompanha as mudanças (até 1632 linhas). ×1 a ×6 desenham "
     "272 linhas por passo, 480×272 vezes o passo, a menos que a proporção seja Preencher. Mais alto fica mais "
     "nítido e exige mais da GPU."},
    {"Display", "Tela"},
    {"Fullscreen", "Tela cheia"},
    {"Window", "Janela"},
    {"Fill the screen, or play in a window you can resize.",
     "Ocupar a tela inteira, ou jogar numa janela que você pode redimensionar."},
    {"Window size", "Tamanho da janela"},
    {"The window's size, in multiples of the PSP's screen.", "O tamanho da janela, em múltiplos da tela do PSP."},
    {"Aspect ratio", "Proporção"},
    {"Original", "Original"},
    {"Stretch", "Esticar"},
    {"Fill", "Preencher"},
    {"Original keeps the PSP's shape with black bars at the sides or top. Stretch fills the window by stretching "
     "the picture.",
     "Original mantém o formato do PSP com barras pretas nas laterais ou em cima. Esticar preenche a janela "
     "esticando a imagem."},
    {"Original keeps the PSP's shape with black bars at the sides or top. Stretch fills the window by stretching "
     "the picture. Fill widens (or narrows) the game's view to the window's shape, keeping its height, and keeps "
     "the interface in the PSP's proportions.",
     "Original mantém o formato do PSP com barras pretas nas laterais ou em cima. Esticar preenche a janela "
     "esticando a imagem. Preencher alarga (ou estreita) a visão do jogo até o formato da janela, mantendo a "
     "altura, e deixa a interface nas proporções do PSP."},
    {"Scaling filter", "Filtro de escala"},
    {"Antialiasing", "Antisserrilhamento"},
    {"Smooths the jagged edges of the picture. FXAA is a light pass over the finished picture that suits phones too; it softens text a little. Higher internal resolutions smooth edges as well, at a higher cost.",
     "Suaviza as bordas serrilhadas da imagem. O FXAA é uma passada leve sobre a imagem pronta que serve também para celulares; ele suaviza um pouco o texto. Resoluções internas maiores também suavizam as bordas, a um custo maior."},
    {"Sharp", "Nítido"},
    {"Smooth", "Suave"},
    {"How the finished picture is scaled to the window: smooth (bilinear) or sharp (nearest pixel).",
     "Como a imagem final é ajustada à janela: suave (bilinear) ou nítida (pixel mais próximo)."},
    {"Texture filter", "Filtro de texturas"},
    {"How the game's textures are sampled: smooth (bilinear) or sharp (nearest texel).",
     "Como as texturas do jogo são amostradas: suave (bilinear) ou nítida (texel mais próximo)."},
    {"Texture pack", "Pacote de texturas"},
    {"Draws an HD texture pack in PPSSPP's format from mods/textures/{} in the data folder instead of the game's "
     "textures.",
     "Desenha um pacote de texturas HD no formato do PPSSPP, de mods/textures/{} na pasta de dados, no lugar das "
     "texturas do jogo."},
    {"Texture packs", "Pacotes de texturas"},
    {"Not installed", "Não instalado"},
    {"No pack in {}", "Nenhum pacote em {}"},
    {"Import texture pack…", "Importar pacote de texturas…"},
    {"Open the textures folder", "Abrir a pasta de texturas"},
    {"Show mods/textures in the data folder, where imported packs go.",
     "Mostra mods/textures, dentro da pasta de dados, para onde vão os pacotes importados."},
    {"Timing", "Tempo"},
    {"Vsync", "Vsync"},
    {"Off (mailbox)", "Desligado (mailbox)"},
    {"Off (immediate)", "Desligado (imediato)"},
    {"On waits for the display's refresh and never tears. Off shows each frame at once; the game's speed is the "
     "same either way.",
     "Ligado espera a atualização da tela e nunca rasga a imagem. Desligado mostra cada quadro na hora; a "
     "velocidade do jogo é a mesma nos dois casos."},
    {"Frame rate", "Taxa de quadros"},
    {"Match display", "Igual à tela"},
    {"With Vsync on, no faster than the display refreshes.", "Com o Vsync ligado, não passa da taxa da tela."},
    {"Lowered to keep the game at full speed; it tries the chosen rate again later.",
     "Reduzida para manter o jogo na velocidade normal; a taxa escolhida é tentada de novo depois."},
    {"Lower when behind", "Reduzir quando atrasar"},
    {"On lowers the frame rate by itself when presenting that often would slow the game. Off keeps the chosen "
     "rate, and the game may then run below full speed.",
     "Ligado reduz a taxa de quadros sozinho quando mostrar tantos quadros deixaria o jogo lento. Desligado "
     "mantém a taxa escolhida, e o jogo pode rodar abaixo da velocidade normal."},
    {"Game speed", "Velocidade do jogo"},
    {"Game speed is Unlimited", "A velocidade do jogo está Ilimitada"},
    {"Unlimited", "Ilimitada"},
    {"Normal", "Normal"},
    {"Normal holds the game to real time. Unlimited lets it run as fast as frames can be drawn, which also speeds "
     "up the game.",
     "Normal mantém o jogo em tempo real. Ilimitada deixa rodar tão rápido quanto os quadros puderem ser "
     "desenhados, o que também acelera o jogo."},
    {"Frame skip", "Pular quadros"},
    {"Limit to 30 fps", "Limitar a 30 fps"},
    {"Limit to 30 fps is on", "Limite de 30 fps ligado"},
    {"Draws every other frame: the game keeps its full speed and the picture shows 30 frames a second, with half "
     "the drawing work. For devices that cannot hold 60.",
     "Desenha um quadro sim, outro não: o jogo mantém a velocidade normal e a imagem mostra 30 quadros por segundo, "
     "com metade do trabalho de desenho. Para aparelhos que não seguram 60."},
    {"Auto: when the game falls behind real time, the next frame is not drawn, so the game keeps its speed and only "
     "the picture shows fewer frames (never fewer than half). Off: every frame is drawn, and a slow scene runs slower.",
     "Auto: quando o jogo fica atrás do tempo real, o próximo quadro não é desenhado, então o jogo mantém a "
     "velocidade e só a imagem mostra menos quadros (nunca menos da metade). Desligado: todo quadro é desenhado, e "
     "uma cena pesada fica mais lenta."},
    {"Fast loading", "Carregamento rápido"},
    {"Lets the game run ahead of real time while it loads and is silent, so loads take as long as the computer "
     "needs. Never during play, sound, movies or ad hoc play.",
     "Deixa o jogo correr à frente do tempo real enquanto carrega em silêncio, então os carregamentos levam só o "
     "tempo que o computador precisa. Nunca durante o jogo, som, vídeos ou partidas ad hoc."},
    {"Performance", "Desempenho"},
    {"Overlay", "Sobreposição"},
    {"Overlay and log", "Sobreposição e log"},
    {"Log only", "Só log"},
    {"Frame times and speed in the top-left corner, and a [perf] line per second on the console. F3 shows or hides "
     "the overlay.",
     "Tempos de quadro e velocidade no canto superior esquerdo, e uma linha [perf] por segundo no console. F3 "
     "mostra ou esconde a sobreposição."},
    {"Restore video defaults", "Restaurar padrões de vídeo"},
    {"Every setting on this page back to how {} ships.", "Todas as opções desta página voltam ao padrão do {}."},

    // Fonts.
    {"Text", "Texto"},
    {"Font", "Fonte"},
    {"Weight", "Peso"},
    {"Regular", "Normal"},
    {"Bold", "Negrito"},
    {"Heavy", "Extra negrito"},
    {"Default (chosen font unreadable)", "Padrão (fonte escolhida ilegível)"},
    {"Thickens the strokes of the game's text. The game squeezes Latin letters to half width, which thins them; "
     "Bold makes up for it.",
     "Engrossa os traços do texto do jogo. O jogo comprime as letras latinas à metade da largura, o que as "
     "afina; Negrito compensa isso."},
    {"Open the fonts folder", "Abrir a pasta de fontes"},
    {"The game's text changes as soon as you choose. Characters a font lacks come from the default; fonts in the "
     "fonts folder are listed first.",
     "O texto do jogo muda assim que você escolhe. Os caracteres que faltam numa fonte vêm da padrão; as fontes "
     "da pasta fonts aparecem primeiro."},
    {"Japanese", "Japonês"},
    {"Your font, Japanese", "Sua fonte, japonês"},
    {"Your font", "Sua fonte"},
    {"Latin", "Latino"},
    {"Looking for installed fonts…", "Procurando fontes instaladas…"},

    // Audio.
    {"Output", "Saída"},
    {"Volume", "Volume"},
    {"Loudness of everything the game plays.", "O volume de tudo que o jogo toca."},
    {"Mute", "Mudo"},
    {"Silence the game without losing the volume setting.", "Silencia o jogo sem perder a configuração de volume."},
    {"Device", "Dispositivo"},
    {"No audio device", "Nenhum dispositivo de áudio"},
    {"44100 Hz stereo, paused while this menu is open", "44100 Hz estéreo, pausado enquanto este menu está aberto"},
    {"44100 Hz stereo", "44100 Hz estéreo"},
    {"Restore audio defaults", "Restaurar padrões de áudio"},
    {"Full volume, not muted.", "Volume máximo, sem mudo."},

    // Controls.
    {"Gamepad", "Controle"},
    {"Connected", "Conectado"},
    {"No gamepad; the keyboard and mouse drive the game", "Nenhum controle; o teclado e o mouse comandam o jogo"},
    {"Confirm button", "Botão de confirmar"},
    {"Bottom (Western)", "Inferior (ocidental)"},
    {"Right (Japanese)", "Direito (japonês)"},
    {"Which face button confirms, in the game and in this menu. The game's prompts show × to confirm and ○ to go "
     "back.",
     "Qual botão frontal confirma, no jogo e neste menu. Os avisos do jogo mostram × para confirmar e ○ para "
     "voltar."},
    {"Which face button confirms, in the game and in this menu. The game's prompts show ○ to confirm and × to go "
     "back.",
     "Qual botão frontal confirma, no jogo e neste menu. Os avisos do jogo mostram ○ para confirmar e × para "
     "voltar."},
    {"Stick dead zone", "Zona morta do analógico"},
    {"How far the left stick moves before the game sees it. Raise it if the character drifts.",
     "Quanto o analógico esquerdo se move antes de o jogo perceber. Aumente se o personagem andar sozinho."},
    {"Trigger point", "Ponto dos gatilhos"},
    {"How far LT/RT (L2/R2) travel before they press anything.",
     "Quanto LT/RT (L2/R2) precisam descer antes de apertar algo."},
    {"Trigger profile", "Perfil dos gatilhos"},
    {"Standard (L / R)", "Padrão (L / R)"},
    {"What LT/RT (L2/R2) press. Standard: L and R, like the shoulders.",
     "O que LT/RT (L2/R2) apertam. Padrão: L e R, como os botões de ombro."},
    {"Right stick", "Analógico direito"},
    {"Camera", "Câmera"},
    {"D-pad", "Direcional"},
    {"Analog camera", "Câmera analógica"},
    {"Camera speed", "Velocidade da câmera"},
    {"How fast the camera turns at full deflection, in degrees a second.",
     "A velocidade com que a câmera gira com o analógico no máximo, em graus por segundo."},
    {"Invert camera horizontally", "Inverter câmera na horizontal"},
    {"Turn the camera the other way left and right.", "Gira a câmera no sentido contrário para os lados."},
    {"Invert camera vertically", "Inverter câmera na vertical"},
    {"Turn the camera the other way up and down.", "Gira a câmera no sentido contrário para cima e para baixo."},
    {"Right stick is not the camera", "O analógico direito não é a câmera"},
    {"Analog camera is off", "A câmera analógica está desligada"},
    {"Right stick is not the D-pad", "O analógico direito não é o direcional"},
    {"Right stick D-pad point", "Ponto do direcional no analógico direito"},
    {"How far the right stick moves before it presses the D-pad.",
     "Quanto o analógico direito se move antes de apertar o direcional."},
    {"When the game asks for a name", "Quando o jogo pedir um nome"},
    {"On-screen keyboard", "Teclado na tela"},
    {"Use the name below", "Usar o nome abaixo"},
    {"On-screen keyboard: type the name with the gamepad or the keyboard while the game waits. Otherwise the name "
     "below is given at once.",
     "Teclado na tela: digite o nome com o controle ou o teclado enquanto o jogo espera. Senão, o nome abaixo é "
     "usado na hora."},
    {"Keyboard and mouse", "Teclado e mouse"},
    {"While the game runs, the window takes the pointer: moving the mouse turns the camera and its buttons press "
     "what they are bound to. Esc opens this menu and gives the pointer back.",
     "Enquanto o jogo roda, a janela captura o ponteiro: mover o mouse gira a câmera e os botões apertam o que "
     "estiver associado a eles. Esc abre este menu e devolve o ponteiro."},
    {"Mouse", "Mouse"},
    {"Mouse is off", "O mouse está desligado"},
    {"Mouse sensitivity", "Sensibilidade do mouse"},
    {"Invert mouse horizontally", "Inverter mouse na horizontal"},
    {"Turn the camera the other way when the mouse moves sideways.",
     "Gira a câmera no sentido contrário quando o mouse vai para os lados."},
    {"Invert mouse vertically", "Inverter mouse na vertical"},
    {"Push the mouse forward to look down instead of up.", "Empurrar o mouse para a frente olha para baixo, não para cima."},
    {"Press a key or a mouse button", "Aperte uma tecla ou um botão do mouse"},
    {"Press a key or a mouse button to add it, or one it has already to remove it; Esc cancels. A key taken from "
     "another control leaves that one.",
     "Aperte uma tecla ou um botão do mouse para adicionar, ou um que já esteja associado para remover; Esc "
     "cancela. Uma tecla tirada de outro comando deixa aquele comando."},
    {"This menu", "Este menu"},
    {"Performance overlay", "Sobreposição de desempenho"},
    {"Restore control defaults", "Restaurar padrões dos controles"},
    {"Every gamepad, keyboard, mouse, touch and name setting back to how {} ships.",
     "Todas as opções de controle, teclado, mouse, toque e nome voltam ao padrão do {}."},
    {"Touch screen", "Tela de toque"},
    {"On-screen controls", "Controles na tela"},
    {"On-screen controls are off", "Os controles na tela estão desligados"},
    {"Controls opacity", "Opacidade dos controles"},
    {"How strongly the on-screen controls are drawn.", "Quão visíveis os controles na tela ficam."},
    {"Controls size", "Tamanho dos controles"},
    {"The size of the on-screen controls.", "O tamanho dos controles na tela."},
    {"Touch camera speed", "Velocidade da câmera ao tocar"},
    {"Degrees the camera turns for a drag across the height of the screen.",
     "Quantos graus a câmera gira ao arrastar o dedo pela altura da tela."},
    {"Use the classic keyboard layout", "Usar o layout clássico de teclado"},
    {"Tilt controls", "Controles de inclinação"},
    {"Up", "Cima"},
    {"Down", "Baixo"},
    {"Left", "Esquerda"},
    {"Right", "Direita"},
    {"Cross", "Xis"},
    {"Circle", "Círculo"},
    {"Square", "Quadrado"},
    {"Triangle", "Triângulo"},
    {"Start", "Start"},
    {"Select (PSP)", "Select (PSP)"},

    // System.
    {"Game", "Jogo"},
    {"Back to the game.", "Volta para o jogo."},
    {"Pause the game when the menu opens", "Pausar o jogo ao abrir o menu"},
    {"On: the game stops while this menu is open. Off: it keeps running and playing sound behind the menu, which "
     "takes all input. Applies the next time the menu opens.",
     "Ligado: o jogo para enquanto este menu está aberto. Desligado: ele continua rodando e tocando som atrás do "
     "menu, que recebe todos os comandos. Vale na próxima vez que o menu abrir."},
    {"Show the launcher at start", "Mostrar o launcher ao iniciar"},
    {"On: the launcher, with its Play button and settings, comes up before the game. Off: the game starts at once. "
     "Applies the next time the game starts.",
     "Ligado: o launcher, com o botão Jogar e as configurações, aparece antes do jogo. Desligado: o jogo inicia "
     "direto. Vale na próxima vez que o jogo iniciar."},
    {"Language", "Idioma"},
    {"The language of this menu and the launcher. The game's own text is not changed.",
     "O idioma deste menu e do launcher. O texto do próprio jogo não muda."},
    {"Open the data folder", "Abrir a pasta de dados"},
    {"Show {}'s data folder in the file manager.", "Mostra a pasta de dados do {} no gerenciador de arquivos."},
    {"Set up game data again…", "Configurar os dados do jogo de novo…"},
    {"Choose the disc image again, for example after moving it. The game closes first.",
     "Escolha a imagem do disco de novo, por exemplo depois de movê-la. O jogo fecha antes."},
    {"Quit game", "Sair do jogo"},
    {"Close {}. Progress since your last save is lost.", "Fecha o {}. O progresso desde o último save é perdido."},
    {"Saves", "Saves"},
    {"About", "Sobre"},
    {"Build", "Build"},
    {"Data folder", "Pasta de dados"},
    {"Saves folder", "Pasta de saves"},
    {"Graphics", "Gráficos"},
    {"Interface", "Interface"},
    {"Quit the game?", "Sair do jogo?"},
    {"Set up game data again?", "Configurar os dados do jogo de novo?"},
    {"Progress since your last save is lost.", "O progresso desde o último save é perdido."},
    {"{} closes the game and opens the setup, where you choose the disc image again. Progress since your last save "
     "is lost.",
     "O {} fecha o jogo e abre a configuração, onde você escolhe a imagem do disco de novo. O progresso desde o "
     "último save é perdido."},
    {"Close and set up", "Fechar e configurar"},
    {"Set by {}", "Definido por {}"},
    {"Save the log…", "Salvar o log…"},
    {"Log", "Log"},

    // Saves.
    {"Import save…", "Importar save…"},
    {"Export save…", "Exportar save…"},
    {"Back up saves…", "Fazer backup dos saves…"},
    {"Open the saves folder", "Abrir a pasta de saves"},
    {"Show the folder the game saves to (PSP/SAVEDATA) in the file manager.",
     "Mostra no gerenciador de arquivos a pasta onde o jogo salva (PSP/SAVEDATA)."},
    {"Copy a save from a PSP memory stick, PPSSPP or another installation: choose its folder or the PSP/SAVEDATA "
     "folder that holds it. A save it replaces is kept, not deleted.",
     "Copia um save de um memory stick de PSP, do PPSSPP ou de outra instalação: escolha a pasta dele ou a pasta "
     "PSP/SAVEDATA que o contém. Um save substituído é guardado, não apagado."},
    {"Copy your game data and downloaded quests to a folder you choose, to take them to a PSP or another machine.",
     "Copia seus dados de jogo para uma pasta que você escolher, para levar a um PSP ou a outro computador."},
    {"Copy all of this game's saves, the install data included, to the backups folder or a folder you choose.",
     "Copia todos os saves deste jogo, incluindo os dados de instalação, para a pasta de backups ou uma pasta "
     "que você escolher."},
    {"No folders here.", "Nenhuma pasta aqui."},
    {"Import from this folder", "Importar desta pasta"},
    {"Export to this folder", "Exportar para esta pasta"},
    {"Back up to this folder", "Fazer backup nesta pasta"},
    {"Import: open a save folder, or choose a folder that holds them, such as a memory stick's PSP/SAVEDATA.",
     "Importar: abra uma pasta de save, ou escolha uma pasta que contenha saves, como a PSP/SAVEDATA de um "
     "memory stick."},
    {"Export: choose the folder to copy your saves to.", "Exportar: escolha a pasta para onde copiar seus saves."},
    {"Back up: choose the folder the backup goes to.", "Backup: escolha a pasta onde o backup vai ficar."},
    {"Import saves", "Importar saves"},
    {"Save to import", "Save a importar"},
    {"Current save", "Save atual"},
    {"Replace and import", "Substituir e importar"},
    {"Import this save", "Importar este save"},
    {"Import these saves", "Importar estes saves"},
    {"Choose another folder", "Escolher outra pasta"},
    {"Back to the folders.", "Volta para as pastas."},
    {"Import nothing.", "Não importa nada."},
    {"Imported", "Importado"},
    {"Imported; the old save was kept", "Importado; o save antigo foi guardado"},
    {"The game reads its saves at the title screen. Restart now to load the imported save; progress since your "
     "last save is lost. If you keep playing instead, do not save before you restart: saving would replace the "
     "imported save with the game you are playing.",
     "O jogo lê os saves na tela de título. Reinicie agora para carregar o save importado; o progresso desde o "
     "último save é perdido. Se continuar jogando, não salve antes de reiniciar: salvar substituiria o save "
     "importado pelo jogo atual."},
    {"Restart now", "Reiniciar agora"},
    {"Closes the game and starts it again at the title screen.", "Fecha o jogo e o inicia de novo na tela de título."},
    {"Back to the menu.", "Volta para o menu."},
    {"Export saves", "Exportar saves"},
    {"Exported", "Exportado"},
    {"Encrypted for a PSP", "Criptografado para PSP"},
    {"To", "Para"},
    {"The folder is laid out like a memory stick: copy its PSP folder to the root of a PSP's memory stick, or "
     "import it from this menu on another machine.",
     "A pasta segue a estrutura de um memory stick: copie a pasta PSP dela para a raiz do memory stick de um "
     "PSP, ou importe por este menu em outro computador."},
    {"Not exported", "Não exportado"},
    {"Back up saves", "Backup dos saves"},
    {"There is no save to back up yet.", "Ainda não há save para fazer backup."},
    {"Add a timestamp to the backup name", "Pôr data e hora no nome do backup"},
    {"On: every backup is a new folder named by its date and time, such as 2026-09-19_19-05-12, holding the save "
     "folders. Off: the save folders go straight into the folder you choose, and an earlier backup there is "
     "replaced after asking.",
     "Ligado: cada backup é uma pasta nova com a data e a hora, como 2026-09-19_19-05-12, com as pastas de save "
     "dentro. Desligado: as pastas de save vão direto para a pasta escolhida, e um backup anterior ali é "
     "substituído depois de perguntar."},
    {"Back up to the backups folder", "Backup na pasta de backups"},
    {"Back up to another folder…", "Backup em outra pasta…"},
    {"Choose where the backup goes.", "Escolha onde o backup vai ficar."},
    {"Open the backups folder", "Abrir a pasta de backups"},
    {"Replace the earlier backup?", "Substituir o backup anterior?"},
    {"Keep the earlier backup.", "Mantém o backup anterior."},
    {"Replace the backup", "Substituir o backup"},
    {"The earlier backup there is replaced.", "O backup anterior ali é substituído."},
    {"Backed up", "Backup feito"},
    {"Not backed up", "Backup não feito"},
    {"Open the folder", "Abrir a pasta"},
    {"Show the backup in the file manager.", "Mostra o backup no gerenciador de arquivos."},

    // Texture packs.
    {"Import texture pack", "Importar pacote de texturas"},
    {"Pack to import", "Pacote a importar"},
    {"No texture pack in this folder", "Nenhum pacote de texturas nesta pasta"},
    {"Ready to install", "Pronto para instalar"},
    {"This pack cannot be used", "Este pacote não pode ser usado"},
    {"{} textures, {}", "{} texturas, {}"},
    {"made for {}, works on {}", "feito para {}, funciona no {}"},
    {"{} images missing: those textures keep the game's own look.",
     "{} imagens faltando: essas texturas ficam com a aparência original."},
    {"Install", "Instalar"},
    {"Copies {} to mods/textures/{} in the data folder ({} free).",
     "Copia {} para mods/textures/{} na pasta de dados ({} livres)."},
    {"Copies {} to mods/textures/{} in the data folder.", "Copia {} para mods/textures/{} na pasta de dados."},
    {"The pack there now is kept as a backup.", "O pacote que está lá é guardado como backup."},
    {"Not enough free space: the copy needs {} and {} is free. Use it where it is instead.",
     "Sem espaço livre suficiente: a cópia precisa de {} e há {} livres. Use-o onde está."},
    {"Copy into the data folder", "Copiar para a pasta de dados"},
    {"Reads the pack from its folder and copies nothing, saving {}. The folder must stay where it is.",
     "Lê o pacote da pasta dele, sem copiar nada, economizando {}. A pasta precisa ficar onde está."},
    {"Does not load, {}", "Não carrega, {}"},
    {"Folder", "Pasta"},
    {"Textures", "Texturas"},
    {"Images", "Imagens"},
    {"Missing images", "Imagens faltando"},
    {"textures.ini names images that are not in the folder; those textures keep the game's own look.",
     "O textures.ini cita imagens que não estão na pasta; essas texturas ficam com a aparência original do jogo."},
    {"In use now", "Em uso agora"},
    {"Installed pack", "Pacote instalado"},
    {"Pack folder", "Pasta do pacote"},
    {"Used from", "Usado de"},
    {"This is the installed pack already.", "Este já é o pacote instalado."},
    {"Copy and replace", "Copiar e substituir"},
    {"This pack is the one in use already.", "Este pacote já é o que está em uso."},
    {"Use it where it is", "Usar onde está"},
    {"Putting the pack in place", "Colocando o pacote no lugar"},
    {"Copying texture pack", "Copiando o pacote de texturas"},
    {"The pack in use now stays as it is until the copy is complete. The menu stays open until then.",
     "O pacote em uso continua como está até a cópia terminar. O menu fica aberto até lá."},
    {"Cancelling…", "Cancelando…"},
    {"Stops the copy and removes what it has copied so far. Nothing else changes.",
     "Para a cópia e remove o que já foi copiado. Nada mais muda."},
    {"Installed", "Instalado"},
    {"Installed in", "Instalado em"},
    {"Replaced pack", "Pacote substituído"},
    {"The pack is on and applies at once: textures already on screen change within a frame or two.",
     "O pacote está ligado e vale na hora: as texturas na tela mudam em um ou dois quadros."},
    {"Not imported", "Não importado"},
    {"Cancelled", "Cancelado"},
    {"Nothing changed: the pack in use before is still in place.",
     "Nada mudou: o pacote que estava em uso continua no lugar."},
    {"Show the folder the pack is in.", "Mostra a pasta onde o pacote está."},
    {"Pack used from", "Pacote usado de"},
    {"Stop using the pack folder", "Parar de usar a pasta do pacote"},
    {"Go back to the pack installed in the data folder, if any. The pack's own folder is left alone.",
     "Volta para o pacote instalado na pasta de dados, se houver. A pasta do pacote não é mexida."},
    {"Does not load", "Não carrega"},
    {"Install an HD texture pack from a folder: the one that holds textures.ini, or one that holds it in "
     "textures/{} or PSP/TEXTURES. It is checked first; the pack it replaces is kept.",
     "Instala um pacote de texturas HD de uma pasta: a que contém o textures.ini, ou uma que o contenha em "
     "textures/{} ou PSP/TEXTURES. Ele é verificado antes; o pacote substituído é guardado."},
    {"Pick the pack's folder (the one that holds textures.ini, or one that holds it in PSP/TEXTURES/{}) in "
     "Android's file picker. It is copied into the game's data folder and checked; the pack it replaces is kept.",
     "Escolha a pasta do pacote (a que contém o textures.ini, ou uma que o contenha em PSP/TEXTURES/{}) no "
     "seletor de arquivos do Android. Ele é copiado para a pasta de dados do jogo e verificado; o pacote "
     "substituído é guardado."},
    {"Looking for the pack…", "Procurando o pacote…"},
    {"Import: open the pack's folder, the one that holds textures.ini, or choose a folder that holds it, such as "
     "PPSSPP's PSP/TEXTURES. You can also drop the folder on the window.",
     "Importar: abra a pasta do pacote, a que contém o textures.ini, ou escolha uma pasta que a contenha, como a "
     "PSP/TEXTURES do PPSSPP. Você também pode arrastar a pasta para a janela."},

    // The file browser.
    {"Home", "Início"},
    {"Desktop", "Área de trabalho"},
    {"Documents", "Documentos"},
    {"Downloads", "Downloads"},
    {"Parent folder", "Pasta acima"},
    {"Showing all files", "Mostrando todos os arquivos"},
    {"System dialog…", "Janela do sistema…"},
    {"Disc images (*.iso)", "Imagens de disco (*.iso)"},
    {"All files", "Todos os arquivos"},

    // The launcher.
    {"PLAY", "JOGAR"},
    {"QUICK SETTINGS", "CONFIGURAÇÕES RÁPIDAS"},
    {"Back up saves before playing", "Backup dos saves ao jogar"},
    {"Copies your saves to the backups folder in the data folder each time you press Play; the newest ten are kept.",
     "Copia seus saves para a pasta backups, na pasta de dados, sempre que você clica em Jogar; os dez mais "
     "recentes são mantidos."},
    {"Show this launcher at start", "Mostrar este launcher ao iniciar"},
    {"No: the game starts at once. Start with --launcher to see this screen again.",
     "Não: o jogo inicia direto. Inicie com --launcher para ver esta tela de novo."},
    {"In the game: Esc, or L3+R3 on a gamepad, opens the port's menu.",
     "No jogo: Esc, ou L3+R3 no controle, abre o menu do port."},
    {"Project page", "Página do projeto"},
    {"Report a problem", "Reportar um problema"},
    {"Your disc image could not be read.", "Não foi possível ler a imagem do disco."},
    {"System & saves", "Sistema e saves"},
    {"An unofficial port, played from your own copy of the game.",
     "Um port não oficial, jogado a partir da sua própria cópia do jogo."},
};

using Table = std::unordered_map<std::string_view, const char *>;

const Table &portuguese() {
    static const Table table = [] {
        Table t;
        for (const Entry &e : kPortuguese) t.emplace(e.english, e.portuguese);
        return t;
    }();
    return table;
}

Language system_language() {
    static const Language system = [] {
        int count = 0;
        SDL_Locale **locales = SDL_GetPreferredLocales(&count);
        const bool pt = locales != nullptr && count > 0 && locales[0]->language != nullptr &&
                        std::string_view(locales[0]->language) == "pt";
        SDL_free(locales);
        return pt ? Language::Portuguese : Language::English;
    }();
    return system;
}

} // namespace

Language language() {
    const std::string &chosen = settings::current().language;
    if (chosen == "pt") return Language::Portuguese;
    if (chosen == "en") return Language::English;
    return system_language();
}

void next_language() {
    settings::current().language = language() == Language::Portuguese ? "en" : "pt";
    settings::save();
}

const char *language_name() { return language() == Language::Portuguese ? "Português" : "English"; }

const char *tr(const char *english) {
    if (english == nullptr || language() == Language::English) return english;
    const Table &table = portuguese();
    const auto found = table.find(english);
    return found == table.end() ? english : found->second;
}

std::string tr(const std::string &english) {
    if (english.empty() || language() == Language::English) return english;
    const Table &table = portuguese();
    const auto found = table.find(english);
    return found == table.end() ? english : std::string(found->second);
}

std::string trf(const char *pattern, std::initializer_list<std::string> args) {
    const std::string translated = tr(pattern);
    std::string out;
    auto arg = args.begin();
    for (std::size_t i = 0; i < translated.size(); ++i) {
        if (translated.compare(i, 2, "{}") == 0 && arg != args.end()) {
            out += *arg++;
            ++i;
        } else {
            out += translated[i];
        }
    }
    return out;
}

} // namespace portablekit::ui
