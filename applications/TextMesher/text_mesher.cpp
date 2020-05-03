
#include "text_mesher.h"
#include "ftgl/Vectoriser.h"

#include <ft2build.h>
#include <freetype/freetype.h>
#include <freetype/ftglyph.h>

#include <easy3d/core/surface_mesh.h>
#include <easy3d/viewer/tessellator.h>
#include <easy3d/util/logging.h>


namespace easy3d {

#define get_face(x)      (reinterpret_cast<FT_Face>(x))
#define get_face_ptr(x)  (reinterpret_cast<FT_Face*>(&x))

#define get_library(x)      (reinterpret_cast<FT_Library>(x))
#define get_library_ptr(x)  (reinterpret_cast<FT_Library*>(&x))

// The resolution in dpi.
#define RESOLUTION          96

// Used to convert actual font size to nominal size, in 26.6 fractional points.
#define SCALE_TO_F26DOT6    64


    TextMesher::TextMesher(const std::string &font_file, int font_height)
            : font_library_(nullptr)
            , font_face_(nullptr)
            , bezier_steps_(4)
            , prev_char_index_(0)
            , cur_char_index_(0)
            , prev_rsb_delta_(0)
    {
        set_font(font_file, font_height);
    }


    TextMesher::~TextMesher() {
        cleanup();
    }


    void TextMesher::cleanup() {
        FT_Done_Face(get_face(font_face_));
        FT_Done_FreeType(get_library(font_library_));
        font_face_ = nullptr;
        font_library_ = nullptr;
    }


    void TextMesher::set_font(const std::string &font_file, int font_height) {
        cleanup();

        if (FT_Init_FreeType(get_library_ptr(font_library_))) {
            LOG(ERROR) << "failed initializing the FreeType library.";
            ready_ = false;
            return;
        }

        if (FT_New_Face(get_library(font_library_), font_file.c_str(), 0, get_face_ptr(font_face_))) {
            LOG(ERROR) << "failed creating FreeType face (probably a problem with your font file)";
            ready_ = false;
            return;
        }

        if (FT_Set_Char_Size(get_face(font_face_), font_height * SCALE_TO_F26DOT6, font_height * SCALE_TO_F26DOT6, RESOLUTION,
                             RESOLUTION)) {
            LOG(ERROR) << "failed requesting the nominal size (in points) of the characters)";
            ready_ = false;
            return;
        }

        ready_ = true;
    }


    TextMesher::CharContour TextMesher::generate_contours(char ch, float& x, float& y) {
        CharContour char_contour;
        char_contour.character = ch;

        cur_char_index_ = FT_Get_Char_Index(get_face(font_face_), ch);
        if (FT_Load_Glyph(get_face(font_face_), cur_char_index_, FT_LOAD_DEFAULT)) {
            LOG_FIRST_N(ERROR, 1) << "failed loading glyph";
            return char_contour;
        }

        FT_Glyph glyph;
        if (FT_Get_Glyph(get_face(font_face_)->glyph, &glyph)) {
            LOG_FIRST_N(ERROR, 1) << "failed getting glyph";
            return char_contour;
        }

        if (glyph->format != FT_GLYPH_FORMAT_OUTLINE) {
            LOG_FIRST_N(ERROR, 1) << "invalid glyph Format";
            return char_contour;
        }

        if (FT_HAS_KERNING(get_face(font_face_)) && prev_char_index_) {
            FT_Vector kerning;
            FT_Get_Kerning(get_face(font_face_), prev_char_index_, cur_char_index_, FT_KERNING_DEFAULT, &kerning);
            x += kerning.x / SCALE_TO_F26DOT6;
        }

        if (prev_rsb_delta_ - get_face(font_face_)->glyph->lsb_delta >= 32)
            x -= 1.0f;
        else if (prev_rsb_delta_ - get_face(font_face_)->glyph->lsb_delta < -32)
            x += 1.0f;

        prev_rsb_delta_ = get_face(font_face_)->glyph->rsb_delta;

        Vectoriser vectoriser(get_face(font_face_)->glyph, bezier_steps_);
        for (std::size_t c = 0; c < vectoriser.ContourCount(); ++c) {
            const ::Contour *contour = vectoriser.GetContour(c);

            Contour polygon(contour->PointCount());
            polygon.clockwise = contour->GetDirection();

            for (std::size_t p = 0; p < contour->PointCount(); ++p) {
                const double *d = contour->GetPoint(p);
                polygon[p] = vec2(d[0] / SCALE_TO_F26DOT6 + x, d[1] / SCALE_TO_F26DOT6 + y);
            }
            char_contour.push_back(polygon);
        }

        prev_char_index_ = cur_char_index_;
        x += get_face(font_face_)->glyph->advance.x / SCALE_TO_F26DOT6;

        return char_contour;
    }


    void TextMesher::generate_contours(const std::string &text, float x, float y, std::vector<CharContour> &contours) {
        if (!ready_)
            return;

        prev_char_index_ = 0;
        cur_char_index_ = 0;
        prev_rsb_delta_ = 0;

        for (int i = 0; i < text.size(); ++i) {
            const auto &char_contour = generate_contours(text[i], x, y);
            contours.push_back(char_contour);
        }
    }


    bool TextMesher::generate(SurfaceMesh* mesh, const std::string &text, float x, float y, float extrude) {
        if (!ready_)
            return false;

        std::vector<CharContour> characters;
        generate_contours(text, x, y, characters);

        if (characters.empty()) {
            LOG(ERROR) << "no contour generated from the text using the specified font";
            return false;
        }

        auto is_inside = [](const Contour &contour_a, const Contour &contour_b) -> bool {
            for (const auto &p : contour_a) {
                if (!geom::point_in_polygon(p, contour_b))
                    return false;
            }
            return true;
        };

        Tessellator tessellator(true);

        for (const auto &ch :characters) {
            for (int index = 0; index < ch.size(); ++index) {
                const Contour &contour = ch[index];
                for (int p = 0; p < contour.size(); ++p) {
                    const vec3 a(contour[p], 0.0f);
                    const vec3 b(contour[(p + 1) % contour.size()], 0.0f);
                    const vec3 c = a + vec3(0, 0, extrude);
                    const vec3 d = b + vec3(0, 0, extrude);
                    mesh->add_triangle(mesh->add_vertex(c), mesh->add_vertex(b), mesh->add_vertex(a));
                    mesh->add_triangle(mesh->add_vertex(c), mesh->add_vertex(d), mesh->add_vertex(b));
                }

                // create faces for the bottom and top
                if (contour.clockwise) {  // according to FTGL, outer contours are clockwise (and inner ones are counter-clockwise)
                    tessellator.begin_polygon(vec3(0, 0, -1));

                    tessellator.set_winding_rule(Tessellator::NONZERO);  // or POSITIVE
                    tessellator.begin_contour();
                    for (const auto &p : contour)
                        tessellator.add_vertex(vec3(p, 0.0));
                    tessellator.end_contour();

                    for (std::size_t inner_index = 0; inner_index < ch.size(); ++inner_index) {
                        const Contour &inner_contour = ch[inner_index];
                        if (inner_index != index &&
                            inner_contour.clockwise != contour.clockwise &&
                            is_inside(inner_contour, contour))
                        {
                            tessellator.set_winding_rule(Tessellator::ODD);
                            tessellator.begin_contour();
                            for (const auto &p : inner_contour)
                                tessellator.add_vertex(vec3(p, 0.0));
                            tessellator.end_contour();
                        }
                    }

                    tessellator.end_polygon();
                }
            }
        }

        const auto &vertices = tessellator.vertices();
        const int num = tessellator.num_triangles();
        for (int i=0; i<num; ++i) {
            unsigned int a, b, c;
            tessellator.get_triangle(i, a, b, c);
            const vec3 va(vertices[a]->data());
            const vec3 vb(vertices[b]->data());
            const vec3 vc(vertices[c]->data());
            // bottom one
            mesh->add_triangle(mesh->add_vertex(va), mesh->add_vertex(vb), mesh->add_vertex(vc));
            // top one
            mesh->add_triangle(
                    mesh->add_vertex(vec3(vc.x, vc.y, vc.z + extrude)),
                    mesh->add_vertex(vec3(vb.x, vb.y, vb.z + extrude)),
                    mesh->add_vertex(vec3(va.x, va.y, va.z + extrude))
            );
        }

        return true;
    }


    SurfaceMesh *TextMesher::generate(const std::string &text, float x, float y, float extrude) {
        if (!ready_)
            return nullptr;

        SurfaceMesh *mesh = new SurfaceMesh;
        if (generate(mesh, text, x, y, extrude))
            return mesh;
        else {
            delete mesh;
            return nullptr;
        }
    }

}