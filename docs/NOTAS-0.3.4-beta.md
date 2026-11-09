# BOTWRecompAndroid 0.3.4-beta (beta 31)

Pequena correção de compatibilidade para aparelhos com 6 GB de RAM. A 0.3.3 ativava automaticamente o perfil de economia de memória em aparelhos que reportavam menos de 6 GiB de RAM, incluindo modelos vendidos com 6 GB. Alguns usuários relataram erros que não ocorriam antes. Agora o perfil automático é reservado aos aparelhos com até 4 GB; os de 6 GB voltam às configurações normais de texturas, buffers e cache. A opção manual `LARECOMP_LOW_RAM` continua disponível, e o limite de pipelines para GPUs Mali permanece independente dessa mudança.

Esta correção foi compilada, mas ainda precisa ser confirmada nos aparelhos de 6 GB afetados. Se o problema continuar, abra uma issue com o modelo, GPU, driver e log.

Instale por cima da 0.2.x ou 0.3.x; não é necessário recompilar o código do jogo. O projeto continua em beta e nenhum arquivo do jogo é incluído.

---

# BOTWRecompAndroid 0.3.4-beta (beta 31)

A small compatibility hotfix for 6 GB devices. Version 0.3.3 automatically enabled the memory-saving profile on devices reporting less than 6 GiB of RAM, including phones sold with 6 GB. Some users reported errors that did not occur before. The automatic profile is now limited to devices with up to 4 GB; 6 GB devices return to the normal texture, buffer, and cache settings. The manual `LARECOMP_LOW_RAM` override remains available, and Mali's separate pipeline limit is unchanged.

The fix has been built but still needs confirmation on affected 6 GB devices. If the problem persists, please open an issue with your device model, GPU, driver, and log.

Install over 0.2.x or 0.3.x without rebuilding the game code. The project is still in beta, and no game files are included.
