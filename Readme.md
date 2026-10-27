<p align="center">
  <img src="Assets/icon.png" width="180" alt="NX-Matrix icon">
</p>

<h1 align="center">NX-Matrix</h1>

<p align="center">
  An open-source, secure and good-looking <a href="https://matrix.org/">Matrix</a> client for the Nintendo Switch
</p>

<p align="center">
  <img src="https://img.shields.io/badge/platform-Nintendo%20Switch-E60012?logo=nintendoswitch&logoColor=white" alt="Platform: Nintendo Switch">
  <img src="https://img.shields.io/badge/language-C%2B%2B-00599C?logo=cplusplus&logoColor=white" alt="Language: C++">
  <img src="https://img.shields.io/badge/Matrix-protocol-000000?logo=matrix&logoColor=white" alt="Matrix protocol">
  <img src="https://img.shields.io/badge/license-GPLv3-blue" alt="License: GPLv3">
  <img src="https://img.shields.io/badge/homebrew-devkitPro-orange" alt="Homebrew: devkitPro">
</p>

> **Note:** This homebrew has only been tested on **firmware 18.0.1** (ou laisse 23.0.1 si c'est voulu, mais attention aux fautes de frappe !) with the latest version of **Atmosphere** (on a Switch V1). Do **not** submit issues if you encounter any error on untested or unsupported configurations.
> Run it **outside** Applet Mode (by holding **R** while launching any game, you can open the HBMenu in Application Mode).
> It currently works on emulators, but is primarily made for real hardware.

### Features :

- Support for many Matrix protocol features (tested for compatibility with the matrix.org homeserver). Note that voice/audio features are not implemented.
- Spaces (m.space, spec v1.19): dedicated section in the room list, rooms grouped under their space, space view with spec-ordered children (`m.space.child`), `/hierarchy` details for unjoined rooms, join nested rooms/subspaces, add/remove rooms, and create spaces.
- Room avatars (`m.room.avatar`) downloaded and displayed in the room list.
- Inline image previews for attachments in the timeline (plain and E2EE media).
- Native Plutonium UI.
- Encrypted data (secured with a PIN/password at startup).

### To Do :

- [ ] Tesla Overlay

### Credits :

- [Plutonium](https://github.com/XorTroll/Plutonium.git) -> UI Rendering
- [SvgRepo](https://www.svgrepo.com/) -> Icons
- [Matrix Specs doc](https://spec.matrix.org/v1.19/client-server-api/) -> Protocol logic *(ou "Core logic" au lieu de "Sounds logic")*
- And other tools used to make Homebrew (like [devkitPro](https://devkitpro.org/) & [libnx](https://github.com/switchbrew/libnx)).

---

### License :

This project is licensed under the GPLv3 License - see the `LICENSE` file for details.
