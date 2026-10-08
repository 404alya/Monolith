# R3D Roadmap

## **v0.12**

* [ ] **Redesign the lighting API**
  Rewrite the lighting API to use a stateless system, allowing users to fill in their own structures and submit them every frame, similarly to draw calls.

* [x] **Review and harmonize OOM handling**
  Currently, each "module" handles allocation failures differently, sometimes logging them and sometimes ignoring them.
  This should be made consistent, possibly following raylib's approach by doing nothing and letting the program fail.

* [x] **Review the color space system**
  The entire system can be simplified by treating `Color` as always being in sRGB and vectors as always being in linear space.
  Similarly for textures: if they contain colors and are RGB(A), always load them as sRGB.
  Also add automatic conversion for RGBA8 attributes (instances/vertices).

* [x] **Transparent Background**
  Allow the use of a transparent background color, making it transparent by default.
  We could either keep the background color as an option, or simply remove it and let the buffer being rendered to determine the background when no skybox is present.

* [ ] **Work on a more efficient method for illumination probes**
  This could use spherical harmonics, or simply the same approach with fewer iterations. It will depend on what proves feasible.

* [ ] **Adding an optional scene context system**
  See this discussion for more details: https://github.com/Bigfoot71/r3d/discussions/288

*Note: This version will mainly focus on preparatory work for the official R3D release.*

## **Ideas (Not Planned Yet)**

* [ ] Add a GL 4.3 build option and implement clustered light culling.
* [ ] Improve support for shadow/transparency interaction (e.g., colored shadows).
* [ ] Implement Cascaded Shadow Maps (or an alternative) for directional lights.
* [ ] Create wiki pages for the repo and consider including them as part of the release.
