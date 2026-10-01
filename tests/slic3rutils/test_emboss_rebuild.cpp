#include <catch2/catch_all.hpp>
#include <boost/filesystem.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/GUI/Jobs/EmbossJob.hpp"

using namespace Slic3r;

namespace {
ModelVolume *add_text(ModelObject &object, const std::string &name, bool per_glyph, bool use_surface, bool with_mesh = false)
{
    ModelVolume *v = object.add_volume(with_mesh ? TriangleMesh(its_make_cube(1., 1., 1.)) : TriangleMesh());
    v->name                        = name;
    v->text_configuration          = TextConfiguration{};
    v->text_configuration->style.prop.per_glyph = per_glyph;
    v->emboss_shape                = EmbossShape{};
    v->emboss_shape->projection.use_surface = use_surface;
    return v;
}

// A file_path style loads a font straight off disk, unlike a wx font descriptor style, which needs
// the font installed and enumerable by wxWidgets -- not available headless in the test binary.
std::string bundled_font_path()
{
    const std::string path = boost::filesystem::path(PROFILES_DIR).parent_path().string() + "/fonts/NotoSansKR-Regular.ttf";
    REQUIRE(boost::filesystem::exists(path));
    return path;
}

// Same as add_text, but with a loadable file_path style, so rebuild_missing_text_meshes() actually
// creates the volume's mesh instead of stopping at "no style to try".
ModelVolume *add_loadable_text(ModelObject &object, const std::string &name, bool per_glyph, bool use_surface, const std::string &font_path)
{
    ModelVolume *v = add_text(object, name, per_glyph, use_surface);
    v->text_configuration->text       = "A";
    v->text_configuration->style.type = EmbossStyle::Type::file_path;
    v->text_configuration->style.path = font_path;
    return v;
}
} // namespace

TEST_CASE("Missing text volumes are rebuilt flat first", "[Emboss]")
{
    Model        model;
    ModelObject *object = model.add_object();
    ModelVolume *surface = add_text(*object, "surface", false, true);
    ModelVolume *flat_a  = add_text(*object, "flat_a", false, false);
    ModelVolume *glyph   = add_text(*object, "glyph", true, false);
    ModelVolume *flat_b  = add_text(*object, "flat_b", false, false);
    add_text(*object, "has_mesh", false, false, true);
    object->add_volume(TriangleMesh());

    const std::vector<ModelVolume *> ordered = GUI::Emboss::missing_text_volumes_in_rebuild_order(*object);
    // Flat first, projected ones after, each group in volume order; volumes with a mesh or without text are left out.
    CHECK(ordered == std::vector<ModelVolume *>{flat_a, flat_b, surface, glyph});
}

TEST_CASE("Rebuild warning names the volumes it warns about", "[Emboss]")
{
    GUI::Emboss::RebuildTextsResult result;
    CHECK(result.warning_text().empty());
    result.similar_font.push_back("my text");
    CHECK(result.warning_text().find("my text") != std::string::npos);
}

TEST_CASE("Rebuild creates a mesh for flat text loaded from a font file", "[Emboss]")
{
    const std::string font_path = bundled_font_path();

    Model        model;
    ModelObject *object = model.add_object();
    ModelVolume *flat   = add_loadable_text(*object, "flat", /*per_glyph=*/false, /*use_surface=*/false, font_path);

    const GUI::Emboss::RebuildTextsResult result = GUI::Emboss::rebuild_missing_text_meshes(model);

    CHECK_FALSE(flat->mesh().empty());
    CHECK(result.placeholder.empty());
    CHECK(result.similar_font.empty());
    CHECK(result.flat_fallback.empty());
}

TEST_CASE("Rebuild falls back to a placeholder when the font can't be loaded", "[Emboss]")
{
    Model        model;
    ModelObject *object = model.add_object();
    ModelVolume *broken = add_loadable_text(*object, "broken", /*per_glyph=*/false, /*use_surface=*/false,
                                             "/nonexistent/does-not-exist.ttf");

    const GUI::Emboss::RebuildTextsResult result = GUI::Emboss::rebuild_missing_text_meshes(model);

    // A placeholder mesh is still created, so slicing and export see a real volume.
    CHECK_FALSE(broken->mesh().empty());
    CHECK(result.placeholder == std::vector<std::string>{"broken"});
    CHECK(result.similar_font.empty());
    CHECK(result.flat_fallback.empty());
}

TEST_CASE("Rebuild of a lone use_surface text falls back flat without other parts to project onto", "[Emboss]")
{
    const std::string font_path = bundled_font_path();

    Model        model;
    ModelObject *object  = model.add_object();
    ModelVolume *surface = add_loadable_text(*object, "surface", /*per_glyph=*/false, /*use_surface=*/true, font_path);

    const GUI::Emboss::RebuildTextsResult result = GUI::Emboss::rebuild_missing_text_meshes(model);

    // The only part in the object, so there is nothing to project onto; still gets a flat mesh.
    CHECK_FALSE(surface->mesh().empty());
    // The object's only part is not reported, even though it was created flat.
    CHECK(result.flat_fallback.empty());
}

TEST_CASE("Rebuild of use_surface text over another part cuts a mesh from its surface", "[Emboss]")
{
    const std::string font_path = bundled_font_path();

    Model        model;
    ModelObject *object  = model.add_object();
    ModelVolume *cube    = object->add_volume(TriangleMesh(its_make_cube(40., 40., 10.)));
    cube->name           = "cube";
    ModelVolume *surface = add_loadable_text(*object, "surface", /*per_glyph=*/false, /*use_surface=*/true, font_path);

    const GUI::Emboss::RebuildTextsResult result = GUI::Emboss::rebuild_missing_text_meshes(model);

    CHECK_FALSE(surface->mesh().empty());
    CHECK(result.placeholder.empty());
    CHECK(result.flat_fallback.empty());
}

TEST_CASE("Rebuild reports a use_surface text that finds no surface because a later one is still empty", "[Emboss]")
{
    const std::string font_path = bundled_font_path();

    Model        model;
    ModelObject *object = model.add_object();
    ModelVolume *a       = add_loadable_text(*object, "a", /*per_glyph=*/false, /*use_surface=*/true, font_path);
    ModelVolume *b       = add_loadable_text(*object, "b", /*per_glyph=*/false, /*use_surface=*/true, font_path);

    const GUI::Emboss::RebuildTextsResult result = GUI::Emboss::rebuild_missing_text_meshes(model);

    // "a" is rebuilt first and finds no surface because "b" is still empty; "b" is then cut from "a".
    CHECK_FALSE(a->mesh().empty());
    CHECK_FALSE(b->mesh().empty());
    CHECK(result.flat_fallback == std::vector<std::string>{"a"});
}

TEST_CASE("Rebuild of per-glyph text without a mesh", "[Emboss]")
{
    const std::string font_path = bundled_font_path();

    Model        model;
    ModelObject *object = model.add_object();
    ModelVolume *cube   = object->add_volume(TriangleMesh(its_make_cube(40., 40., 10.)));
    cube->name          = "cube";
    ModelVolume *glyph  = add_loadable_text(*object, "glyph", /*per_glyph=*/true, /*use_surface=*/false, font_path);

    const GUI::Emboss::RebuildTextsResult result = GUI::Emboss::rebuild_missing_text_meshes(model);

    CHECK_FALSE(glyph->mesh().empty());
    CHECK(result.placeholder.empty());
}
