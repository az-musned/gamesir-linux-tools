# Credits

Deadband is MIT-licensed (see [LICENSE](../LICENSE)). It includes the following
third-party asset, used under its own terms:

## GameSir macro flags and block layout

The macro trigger mode (single press / hold), loop flag, loop interval and the
paddle-block geometry (the two turbo bytes before each block, and the final step
slot that has no delay) come from **Pedro Segato's
[gamesir-reverse-engineering](https://github.com/pedrosegato/gamesir-reverse-engineering)**
(MIT). That project extracted them from GameSir Connect 1.16.7 and verified them on
a Cyclone 2. Deadband reimplements the findings; no code from it is included.

## G502 X mouse diagram silhouette

The top-down G502 X outline in [`qml/App/MouseView.qml`](../qml/App/MouseView.qml)
was traced from the 3D model **"Logitech G502 x lightspeed" by Okopchi**
(<https://skfb.ly/p8PWQ>), licensed under
**[Creative Commons Attribution 4.0](https://creativecommons.org/licenses/by/4.0/)**.

The shipped silhouette is an original vector derivative (a normalized point-path);
no source model geometry is redistributed. Attribution is provided here as required
by CC-BY 4.0.
