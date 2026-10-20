#include <boost/filesystem/operations.hpp>
#include <catch2/catch_all.hpp>
#include <boost/filesystem.hpp>

#include "catch2/catch_test_macros.hpp"
#include "catch2/matchers/catch_matchers.hpp"
#include "catch2/matchers/catch_matchers_vector.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/Jobs/EmbossJob.hpp"
#include "slic3r/Utils/WxFontUtils.hpp"
#include "test_utils.hpp"

#include <string>
#include <libslic3r/TextConfiguration.hpp>
#include <libslic3r/EmbossShape.hpp>
#include <vector>
#include <libslic3r/Point.hpp>
#include <wx/font.h>

using namespace Slic3r;

namespace {
ModelVolume *add_text(ModelObject &object, const std::string &name, bool per_glyph, bool use_surface)
{
    ModelVolume *v = object.add_volume(TriangleMesh());
    v->name                        = name;
    v->text_configuration          = TextConfiguration{};
    v->text_configuration->style.prop.per_glyph = per_glyph;
    v->emboss_shape                = EmbossShape{};
    v->emboss_shape->projection.use_surface = use_surface;
    return v;
}

// Point resources_dir() at the repo's own tree, where the bundled fonts are, and restore it afterwards.
struct ScopedRepoResourcesDir
{
    std::string previous{resources_dir()};

    ScopedRepoResourcesDir() { set_resources_dir(boost::filesystem::path(PROFILES_DIR).parent_path().string()); }
    ~ScopedRepoResourcesDir() { set_resources_dir(previous); }

    ScopedRepoResourcesDir(const ScopedRepoResourcesDir &)            = delete;
    ScopedRepoResourcesDir &operator=(const ScopedRepoResourcesDir &) = delete;
};

// A file_path style of a bundled font loads it straight off disk, unlike a wx font descriptor style, which needs
// the font installed and enumerable by wxWidgets -- not available headless in the test binary.
std::string bundled_font_path()
{
    const std::string path = resources_dir() + "/fonts/NotoSansKR-Regular.ttf";
    REQUIRE(boost::filesystem::exists(path));
    return path;
}

// Same as add_text, but with a loadable file_path style, so rebuild_missing_text_meshes()
// creates the volume's mesh.
ModelVolume *add_loadable_text(ModelObject &object, const std::string &name, bool per_glyph, bool use_surface, const std::string &font_path)
{
    ModelVolume *v = add_text(object, name, per_glyph, use_surface);
    v->text_configuration->text       = "A";
    v->text_configuration->style.type = EmbossStyle::Type::file_path;
    v->text_configuration->style.path = font_path;
    return v;
}
} // namespace

TEST_CASE("Missing text volumes are rebuilt flat first", "[EmbossRebuild]")
{
    Model        model;
    ModelObject *object = model.add_object();
    ModelVolume *surface = add_text(*object, "surface", false, true);
    ModelVolume *flat_a  = add_text(*object, "flat_a", false, false);
    ModelVolume *glyph   = add_text(*object, "glyph", true, false);
    ModelVolume *flat_b  = add_text(*object, "flat_b", false, false);
    add_text(*object, "has_mesh", false, false)->set_mesh(TriangleMesh(its_make_cube(1., 1., 1.)));
    object->add_volume(TriangleMesh());
    // An SVG emboss volume has no text configuration.
    object->add_volume(TriangleMesh())->emboss_shape = EmbossShape{};
    // Only a corrupted file attaches text to a support blocker; is_valid() asserts on that type.
    add_text(*object, "blocker", false, false)->set_type(ModelVolumeType::SUPPORT_BLOCKER);

    const std::vector<ModelVolume *> ordered = GUI::Emboss::missing_text_volumes_in_rebuild_order(*object);
    // Flat first, projected ones after, each group in volume order; volumes with a mesh, without text
    // or of a type text is not created as are left out.
    CHECK(ordered == std::vector<ModelVolume *>{flat_a, flat_b, surface, glyph});
}

TEST_CASE("Rebuild warning names the volumes it warns about", "[EmbossRebuild]")
{
    GUI::Emboss::RebuildTextsResult result;
    CHECK(result.warning_text().empty());
    result.similar_font.push_back("my text");
    CHECK(result.warning_text().find("my text") != std::string::npos);
}

TEST_CASE("Text without a mesh is rebuilt from its text and font", "[EmbossRebuild]")
{
    const ScopedRepoResourcesDir repo_resources;
    const std::string font_path = bundled_font_path();

    Model        model;
    ModelObject *object = model.add_object();

    SECTION("Rebuild creates a mesh for flat text loaded from a font file")
    {
        ModelVolume *flat = add_loadable_text(*object, "flat", /*per_glyph=*/false, /*use_surface=*/false, font_path);

        const GUI::Emboss::RebuildTextsResult result = GUI::Emboss::rebuild_missing_text_meshes(model);

        CHECK_FALSE(flat->mesh().empty());
        CHECK(result.placeholder.empty());
        CHECK(result.similar_font.empty());
        CHECK(result.flat_fallback.empty());
    }

    SECTION("Rebuild uses a similar font when the font file is missing")
    {
        ModelVolume *broken = add_loadable_text(*object, "broken", /*per_glyph=*/false, /*use_surface=*/false,
                                                 "/nonexistent/does-not-exist.ttf");
        // The fallback creates a wx font of the style's family. Without any system font it ends at the GUI font,
        // which GTK aborts on without a display, as wx isn't initialized in this binary.
        if (!GUI::WxFontUtils::create_wxFont(broken->text_configuration->style).IsOk())
            SKIP("no system font for the style's family");

        const GUI::Emboss::RebuildTextsResult result = GUI::Emboss::rebuild_missing_text_meshes(model);

        // Falls back like the gizmo does, instead of a placeholder.
        CHECK_FALSE(broken->mesh().empty());
        CHECK(result.similar_font == std::vector<std::string>{"broken"});
        CHECK(result.placeholder.empty());
        CHECK(result.flat_fallback.empty());
    }

    SECTION("Rebuild of a lone use_surface text falls back flat without other parts to project onto")
    {
        ModelVolume *surface = add_loadable_text(*object, "surface", /*per_glyph=*/false, /*use_surface=*/true, font_path);

        const GUI::Emboss::RebuildTextsResult result = GUI::Emboss::rebuild_missing_text_meshes(model);

        // The only part in the object, so there is nothing to project onto; still gets a flat mesh.
        CHECK_FALSE(surface->mesh().empty());
        // The object's only part is not reported, even though it was created flat.
        CHECK(result.flat_fallback.empty());
    }

    SECTION("Rebuild of use_surface text over another part cuts a mesh from its surface")
    {
        ModelVolume *cube    = object->add_volume(TriangleMesh(its_make_cube(40., 40., 10.)));
        cube->name           = "cube";
        ModelVolume *surface = add_loadable_text(*object, "surface", /*per_glyph=*/false, /*use_surface=*/true, font_path);

        const GUI::Emboss::RebuildTextsResult result = GUI::Emboss::rebuild_missing_text_meshes(model);

        CHECK_FALSE(surface->mesh().empty());
        CHECK(result.placeholder.empty());
        CHECK(result.flat_fallback.empty());
    }

    SECTION("Rebuild reports a use_surface text that finds no surface because a later one is still empty")
    {
        ModelVolume *a = add_loadable_text(*object, "a", /*per_glyph=*/false, /*use_surface=*/true, font_path);
        ModelVolume *b = add_loadable_text(*object, "b", /*per_glyph=*/false, /*use_surface=*/true, font_path);

        const GUI::Emboss::RebuildTextsResult result = GUI::Emboss::rebuild_missing_text_meshes(model);

        // "a" is rebuilt first and finds no surface because "b" is still empty; "b" is then cut from "a".
        CHECK_FALSE(a->mesh().empty());
        CHECK_FALSE(b->mesh().empty());
        CHECK(result.flat_fallback == std::vector<std::string>{"a"});
    }

    SECTION("Rebuild finds a bundled font by its file name, as stored on another machine")
    {
        ModelVolume *flat = add_loadable_text(*object, "flat", /*per_glyph=*/false, /*use_surface=*/false,
                                              "C:\\Program Files\\OrcaSlicer\\resources\\fonts\\NotoSansKR-Regular.ttf");

        const GUI::Emboss::RebuildTextsResult result = GUI::Emboss::rebuild_missing_text_meshes(model);

        CHECK_FALSE(flat->mesh().empty());
        CHECK(result.similar_font.empty());
    }

    SECTION("Rebuild doesn't open a font file that is not bundled")
    {
        const ScopedTemporaryDir      dir("orca_not_bundled_font");
        const boost::filesystem::path copy = dir.path() / "not-bundled.ttf";
        ModelVolume *other = add_loadable_text(*object, "other", /*per_glyph=*/false, /*use_surface=*/false, copy.string());
        // See "Rebuild uses a similar font when the font file is missing"
        if (!GUI::WxFontUtils::create_wxFont(other->text_configuration->style).IsOk())
            SKIP("no system font for the style's family");
        boost::filesystem::copy_file(font_path, copy);

        const GUI::Emboss::RebuildTextsResult result = GUI::Emboss::rebuild_missing_text_meshes(model);

        CHECK(result.similar_font == std::vector<std::string>{"other"});
    }

    SECTION("Rebuild uses the first face when the font has no face of the stored collection number")
    {
        ModelVolume *face = add_loadable_text(*object, "face", /*per_glyph=*/false, /*use_surface=*/false, font_path);
        face->text_configuration->style.prop.collection_number = 5;

        const GUI::Emboss::RebuildTextsResult result = GUI::Emboss::rebuild_missing_text_meshes(model);

        CHECK_FALSE(face->mesh().empty());
        // Substituted for the mesh only, so the face is used again once the font is installed
        CHECK(face->text_configuration->style.prop.collection_number == 5u);
        CHECK(result.similar_font == std::vector<std::string>{"face"});
    }

    SECTION("Rebuild uses a placeholder for text of zero size")
    {
        ModelVolume *zero = add_loadable_text(*object, "zero", /*per_glyph=*/false, /*use_surface=*/false, font_path);
        zero->text_configuration->style.prop.size_in_mm = 0.f;

        const GUI::Emboss::RebuildTextsResult result = GUI::Emboss::rebuild_missing_text_meshes(model);

        CHECK(zero->mesh().its.vertices.size() > 0);
        CHECK(result.placeholder == std::vector<std::string>{"zero"});
    }

    SECTION("Rebuild stops before the next text when canceled")
    {
        ModelVolume *flat = add_loadable_text(*object, "flat", /*per_glyph=*/false, /*use_surface=*/false, font_path);

        GUI::Emboss::rebuild_missing_text_meshes(model, [] { return true; });

        CHECK(flat->mesh().empty());
    }

    SECTION("Rebuild of use_surface text off the part falls back flat")
    {
        ModelVolume *cube    = object->add_volume(TriangleMesh(its_make_cube(40., 40., 10.)));
        cube->name           = "cube";
        ModelVolume *surface = add_loadable_text(*object, "surface", /*per_glyph=*/false, /*use_surface=*/true, font_path);
        surface->set_transformation(Transform3d(Eigen::Translation3d(100., 0., 0.)));

        const GUI::Emboss::RebuildTextsResult result = GUI::Emboss::rebuild_missing_text_meshes(model);

        // The text doesn't project onto the cube, so cut_surface finds no valid surface; still gets a flat mesh.
        CHECK_FALSE(surface->mesh().empty());
        CHECK(result.flat_fallback == std::vector<std::string>{"surface"});
        CHECK(result.placeholder.empty());
    }

    SECTION("Rebuild reports use_surface text without a shape only as a placeholder")
    {
        object->add_volume(TriangleMesh(its_make_cube(40., 40., 10.)));
        ModelVolume *blank = add_loadable_text(*object, "blank", /*per_glyph=*/false, /*use_surface=*/true, font_path);
        blank->text_configuration->text = " ";

        const GUI::Emboss::RebuildTextsResult result = GUI::Emboss::rebuild_missing_text_meshes(model);

        CHECK_FALSE(blank->mesh().empty());
        CHECK(result.placeholder == std::vector<std::string>{"blank"});
        CHECK(result.flat_fallback.empty());
    }

    SECTION("Rebuild of per-glyph text without a mesh")
    {
        ModelVolume *cube  = object->add_volume(TriangleMesh(its_make_cube(40., 40., 10.)));
        cube->name         = "cube";
        ModelVolume *glyph = add_loadable_text(*object, "glyph", /*per_glyph=*/true, /*use_surface=*/false, font_path);

        const GUI::Emboss::RebuildTextsResult result = GUI::Emboss::rebuild_missing_text_meshes(model);

        CHECK_FALSE(glyph->mesh().empty());
        CHECK(result.placeholder.empty());
    }
}

TEST_CASE("Rebuild removes volumes left without a mesh and objects left without volumes", "[EmbossRebuild]")
{
    // A failed rebuild gets a placeholder mesh, so volumes the rebuild skips stand in for one left empty.
    Model        model;
    ModelObject *kept = model.add_object();
    kept->add_volume(TriangleMesh(its_make_cube(1., 1., 1.)))->name = "cube";
    // Not rebuilt: text on a type text is not created as.
    add_text(*kept, "blocker", false, false)->set_type(ModelVolumeType::SUPPORT_BLOCKER);
    ModelObject *emptied = model.add_object();
    // Not rebuilt: no text configuration.
    emptied->add_volume(TriangleMesh())->name = "empty";

    const GUI::Emboss::RebuildTextsResult result = GUI::Emboss::rebuild_missing_text_meshes(model);

    REQUIRE(model.objects == ModelObjectPtrs{kept});
    REQUIRE(kept->volumes.size() == 1);
    CHECK(kept->volumes.front()->name == "cube");
    CHECK_THAT(result.removed, Catch::Matchers::UnorderedEquals(std::vector<std::string>{"blocker", "empty"}));
    CHECK(result.warning_text().find("blocker") != std::string::npos);
}
