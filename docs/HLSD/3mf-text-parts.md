# Text parts without a mesh in .3mf — High Level Design

## Purpose

A text part may be stored in a .3mf without its mesh. For example, a script
that changes the text will drop the now-stale mesh. A GUI project load rebuilds
such a part from its text configuration and font.

## Importer

- The importer keeps a mesh-less part only when it carries both a text
  configuration and an emboss shape, its final type (after the volume type and
  precise seam metadata) is a part, negative volume or modifier (the types the
  emboss gizmo creates text as), and only for
  `LoadStrategy::KeepEmptyText`. Only `Plater::priv::load_files` sets it,
  because it is the only caller that rebuilds the parts (see Limitations). The
  importer and the rebuild both select these parts with
  `ModelVolume::is_missing_text_mesh()`.
  Every loaded text configuration has an emboss shape: for one saved without
  a shape configuration, as older versions did, the importer builds the shape
  from the text configuration's attributes.
- Any other mesh-less part, including an SVG part and text on a support
  blocker or enforcer from a corrupted file, is dropped by every loader, with a
  log line only; there is no rebuild path for it.

## Rebuild

- `Slic3r::GUI::Emboss::rebuild_missing_text_meshes` rebuilds the meshes before the
  model is validated: per-glyph and use-surface text is projected onto the
  object's other parts, and a font from another OS or one that is not installed
  falls back to a similar font, following the emboss gizmo's wxFont fallback. A
  font stored as a file path is loaded only when Orca bundles a font of that
  file name, from the bundled file; any other falls back like a font that is
  not installed. The path comes from the project, and opening any path it names
  would let a file make Orca read arbitrary files or reach out to a network
  share just by being opened. The gizmo always loads fonts through wxWidgets, so
  editing such a text may switch it to a similar font. A face of a font
  collection that the loaded font doesn't have is replaced by its first face,
  as a substituted font; the stored face is kept for when the font is installed. When the font can't be loaded, or the text has no
  shape or gives non-finite vertices (e.g. a zero size), the gizmo's
  placeholder mesh is used. The user is warned about placeholders and
  substituted fonts.
- The slicing core assumes every volume has a mesh, and fails on an empty
  one before `Print::validate` runs. After the rebuild, any volume still
  without a mesh is therefore removed, together with an object left without
  volumes, and the user is warned about it. With the placeholder this does
  not happen today; it keeps the invariant should a rebuild ever leave a
  volume empty.
- The rebuild runs synchronously on the UI thread, before validation and plate
  placement, while the project loads. The load's progress dialog is updated
  before each text, so canceling it between texts cancels the load; a single
  long per-glyph or use-surface text can't be interrupted.

## Ordering

- Flat texts are rebuilt before per-glyph and use-surface ones, because those need
  the other parts' meshes. Two use-surface texts that lie on each other depend on
  volume order: the one rebuilt first finds no surface and is created flat, as is
  a text whose cut onto the surface fails, and its
  `use_surface` setting is kept until it is re-embossed. The user is warned about
  these texts. The same happens when the object's other parts are only negative
  volumes or modifiers, or when the text does not lie on them. A per-glyph text
  that finds no surface keeps its glyphs on straight lines and is not reported.
  A text that is the object's only part is flat by design and not reported.

## Transform contract

For a part without a mesh the component transform is the text frame, and the
shape's `fix_3mf_tr` is ignored. `fix_3mf_tr` only undoes the centering applied
when a mesh is loaded, which an empty volume does not get.

### Scripting

A script stripping the mesh of a text part saved by Orca must write the component
transform `comp * T(c) * fix^-1` and drop the shape's `transform` attribute. Here
`comp` is the saved component transform, `c` the center of the stripped mesh's
vertex bbox and `fix` the shape's `transform`. A script writing a text part from
scratch writes the text frame directly.

A script choosing a font sets the style's `face_name` to a font installed on the
machines that open the file: the rebuild finds an installed face by that name for
any style type, including a `file_path` style whose file isn't bundled with Orca.
Pointing a `file_path` style at the font file instead only works for a bundled font.

## Limitations

- The CLI, Reload from disk and other loaders drop mesh-less text parts with
  only a log line, so slicing a script-edited file with `--slice` loses the
  text, logged but otherwise silent. The rebuild loads fonts through wxWidgets,
  which the CLI does not initialize.
- A substituted or placeholder font is not written back to the file on disk, but
  the rebuilt mesh is: re-saving the project from the GUI makes the substitution
  or placeholder permanent, since the next load finds a mesh, runs no rebuild,
  and shows no warning — until the text is re-embossed.
