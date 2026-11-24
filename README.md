<p align="center">
  <img src="assets/kobalt.svg" alt="Kobalt" width="320" height="208">
</p>

**Kobalt is an EPUB reader with a Qt reading interface. Its rendering engine is [Blaze](doc/engines.md), Kobalt’s fork of CREngine, which is optimized for low-power, single core ARM chips.**

The reader UI is its own thing, and it comes with a library organization option for platforms where integration with existing library features isn't possible. On Android, for example, it runs as a standalone app, and on Kobo it runs somewhat integrated into Kobo's own OS.

### Which platforms are currently supported?

- **Kobo** (the primary platform; Kobo OS 4.x and Kobo OS 6.x beta)
- **Android** (see also [building for Android](doc/building.md#android))
- **Mac** (for [development](doc/building.md#mac) purposes)

## Installing and removing Kobalt

The easiest way to install is via [the website](https://kobalt.nicoverbruggen.be). 

If you are interested in manually installing Kobalt, please read the [installation instructions](doc/installation.md).

## Issues and contributions

This repository contains Kobalt's public source. Report bugs via the [Discussions](https://github.com/nicoverbruggen/kobalt/discussions) tab here on GitHub. Issues are reserved for contributors for managing the project.

If you have a sample book that exhibits an issue, please attach it if you are permitted to do so, this can help find issues more quickly.

## Acknowledgements

For a complete list, please check out [the acknowledgements](doc/acknowledgements.md) document.

### KOReader

[KOReader](https://koreader.rocks/) is an amazing piece of software, and some of its source is used (the renderer, for example) to make this project possible. A big thanks to the contributors of KOReader. If you feel like Kobalt is too limited in its features, you probably want to be using that instead of this.

## Disclaimers

1. This project is not affiliated with Rakuten Kobo Inc. or the tolino alliance. Kobo and tolino are trademarks of their respective owners.
2. The creator of Kobalt does not encourage or endorse the removal of DRM, and Kobalt does not allow for it. Kobalt only opens DRM-free books. Support your favorite author by purchasing their books.
3. This project was made with the help of agentic engineering with models from OpenAI and Anthropic. I would not have been able to make this project otherwise. Instructions intended for agents are included in [AGENTS.md](AGENTS.md).

## License

Like KOReader, this project is available under an open source license. Kobalt's own source code uses the MIT license in [LICENSE](LICENSE). Blaze and its changes to CREngine use GNU GPL v2 or later, and Qt patches use Qt's licenses. Third-party code and assets retain their own licenses and notices.

The combined package includes GPL and AGPL code and is distributed under GNU AGPL v3, without warranty. Binary releases include the corresponding source and dependency sources.
