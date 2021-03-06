/**
 * MIT License
 *
 * Copyright (c) 2018 Matt Chiasson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 **/
#define MPE_POLY2TRI_IMPLEMENTATION
#define TUNIS_GL_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION

#ifdef _WIN32
#define NOMINMAX
#endif

#ifndef TUNIS_CURVE_RECURSION_LIMIT
#define TUNIS_CURVE_RECURSION_LIMIT 32
#endif

#ifndef TUNIS_MAX_TEXTURE_SIZE
#define TUNIS_MAX_TEXTURE_SIZE 2048
#endif

#ifndef TUNIS_VERTEX_MAX
#define TUNIS_VERTEX_MAX 16384
#endif

#include <Tunis.h>

#include <TunisGL.h>
#include <TunisPaint.h>
#include <TunisPath2D.h>
#include <TunisShaderProgram.h>
#include <TunisSOA.h>
#include <TunisTexture.h>
#include <TunisVertex.h>
#include <TunisFonts_generated.h>

#if defined(TUNIS_PROFILING)
#include <easy/profiler.h>
#endif
#include <glm/gtc/epsilon.hpp>
#include <glm/gtx/exterior_product.hpp>
#include <stb/stb_image.h>

#include <fstream>
#include <map>
#include <thread>

namespace tunis
{
    detail::Math Math;

    namespace detail
    {
        const float Math::PI = glm::pi<float>();

        moodycamel::ConcurrentQueue< std::function<void(ContextPriv*)> > taskQueue(128);

        GraphicStates gfxStates;

        enum DrawOp
        {
            DRAW_FILL,
            DRAW_STROKE,
            DRAW_TEXT_FILL,
            DRAW_TEXT_STROKE
        };

        struct BatchArray : public SoA<ShaderProgram*, Texture*, size_t, size_t, Paint>
        {
            inline ShaderProgram* &program(size_t i) { return get<0>(i); }
            inline Texture* &texture(size_t i) { return get<1>(i); }
            inline size_t &offset(size_t i) { return get<2>(i); }
            inline size_t &count(size_t i) { return get<3>(i); }
            inline Paint &paint(size_t i) { return get<4>(i); }
        };

        struct DrawOpArray : public SoA<DrawOp, Path2D, ContextState>
        {
            inline DrawOp &op(size_t i) { return get<0>(i); }
            inline Path2D &path(size_t i) { return get<1>(i); }
            inline ContextState &state(size_t i) { return get<2>(i); }
        };

        class ContextPriv
        {
        public:
            std::vector<ContextState> states;

            std::vector<std::unique_ptr<Texture>> textures;

            std::unique_ptr<ShaderProgramTexture> programTexture;
            std::unique_ptr<ShaderProgramGradientLinear> programGradientLinear;
            std::unique_ptr<ShaderProgramGradientRadial> programGradientRadial;
            GLuint vao = 0;

            enum {
                VBO = 0,
                IBO = 1
            };
            GLuint buffers[2] = {0, 0};

            int32_t viewWidth = 0;
            int32_t viewHeight = 0;

            uint32_t currentVertexOffset = 0;
            std::vector<uint8_t> vertexBuffer; // write-only interleaved VBO data.
            std::vector<uint16_t> indexBuffer; // write-only

            DrawOpArray renderQueue;
            BatchArray batches;

            float tessTol = 0.25f;
            float distTol = 0.01f;

            const FontRepository *fontRepo = nullptr;
            const Font *currentFont = nullptr;

            using FontGlyphImageCache = std::map<const Glyph*, Image>;

            FontGlyphImageCache fontGlyphImageCache;

            inline ContextPriv()
            {
                auto tunisGL_initialized = tunisGLInit();
                if (!tunisGL_initialized)
                {
                    abort();
                }

                std::ifstream inFontfile;
                inFontfile.open("fonts.tfp", std::ios::binary | std::ios::in);
                if (inFontfile.is_open())
                {
                    inFontfile.seekg(0, std::ios::end);
                    std::streamsize length = inFontfile.tellg();
                    inFontfile.seekg(0, std::ios::beg);
                    char *data = new char[length];
                    inFontfile.read(data, length);
                    inFontfile.close();

                    fontRepo = GetFontRepository(data);

                    // Do not delete data: flatbuffers seems to takes ownership of our data pointer
                }


                // Create a default texture atlas.
#ifdef TUNIS_MAX_TEXTURE_SIZE
                gfxStates.maxTexSize = TUNIS_MAX_TEXTURE_SIZE;
#else
                glGetIntegerv(GL_MAX_TEXTURE_SIZE, &global.maxTexSize);
#endif
                gfxStates.texPadding = gfxStates.maxTexSize/64;

                // pixel width in 16bit.
                gfxStates.pixelWidth = static_cast<float>(0xFFFF) / static_cast<float>(gfxStates.maxTexSize);

                std::unique_ptr<Texture> tex = std::unique_ptr<Texture>(new Texture(gfxStates.maxTexSize, gfxStates.maxTexSize));
                textures.emplace_back(std::move(tex)); // retain

                Gradient::reserve(64);
                Image::reserve(64);
                Paint::reserve(64);
                Path2D::reserve(64);
                renderQueue.reserve(1024);
                batches.reserve(1024);

                vertexBuffer.reserve(TUNIS_VERTEX_MAX*sizeof(VertexTexture));
                indexBuffer.reserve((TUNIS_VERTEX_MAX-2)*3);

                if (tunisGLSupport(GL_VERSION_3_0))
                {
                    // Create a dummy vertex array object (mandatory since GL Core profile)
                    glGenVertexArrays(1, &vao);
                    glBindVertexArray(vao);
                }

                // Create vertex and index buffer objects for the batches
                glGenBuffers(2, buffers);
                glBindBuffer(GL_ARRAY_BUFFER, buffers[ContextPriv::VBO]);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[ContextPriv::IBO]);

                // Initialize our shader programs.
                programTexture = std::unique_ptr<ShaderProgramTexture>(new ShaderProgramTexture());
                programGradientLinear = std::unique_ptr<ShaderProgramGradientLinear>(new ShaderProgramGradientLinear());
                programGradientRadial = std::unique_ptr<ShaderProgramGradientRadial>(new ShaderProgramGradientRadial());

                // Use our default texture program.
                programTexture->useProgram();

                /* set default state */
                glEnable(GL_CULL_FACE);
                glCullFace(GL_BACK);
                glFrontFace(GL_CCW);
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_SCISSOR_TEST);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            }

            inline ~ContextPriv()
            {
                // unload texture data by deleting every potential texture holders.
                textures.resize(0);
                batches.resize(0);

                // unload shader programs
                programTexture.reset();
                programGradientLinear.reset();
                programGradientRadial.reset();

                // unload vertex and index buffers
                glDeleteBuffers(2, buffers);
                buffers[ContextPriv::VBO] = 0;
                buffers[ContextPriv::IBO] = 0;

                // reset global states.
                gfxStates = GraphicStates();

                // reset global GL states
                glBindTexture(GL_TEXTURE_2D , 0);
                if (tunisGLSupport(GL_VERSION_3_0))
                {
                    glBindVertexArray(0);
                }
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

                // shut down GL wrangler
                tunisGLShutdown();
            }

            inline void clearFrame(int32_t fbLeft, int32_t fbTop, int32_t fbWidth, int32_t fbHeight, Color backgroundColor)
            {
                // update the clear color if necessary
                if (gfxStates.backgroundColor != backgroundColor)
                {
                    glClearColor(backgroundColor.r/255.0f,
                                 backgroundColor.g/255.0f,
                                 backgroundColor.b/255.0f,
                                 backgroundColor.a/255.0f);

                    gfxStates.backgroundColor = backgroundColor;
                }

                // update the viewport if necessary
                Viewport viewport(fbLeft, fbTop, fbWidth, fbHeight);
                if (gfxStates.viewport != viewport)
                {
                    glViewport(fbLeft, fbTop, fbWidth, fbHeight);
                    gfxStates.viewport = viewport;
                }

                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            }


            template <typename Vertex_t>
            inline uint16_t addBatch(ShaderProgram *program, Texture *texture, uint32_t vertexCount, uint32_t indexCount, Vertex_t **vout, Index **iout)
            {
                assert(vertexCount >= 3);

                size_t istart = indexBuffer.size();
                size_t iend = istart + indexCount;
                indexBuffer.resize(iend);
                if (iout) *iout = &indexBuffer[istart];

                size_t vstart = vertexBuffer.size();
                size_t vend = vstart + (vertexCount * sizeof(Vertex_t));
                vertexBuffer.resize(vend);
                if (vout) *vout = reinterpret_cast<Vertex_t*>(&vertexBuffer[vstart]);

                uint16_t offset = static_cast<uint16_t>(currentVertexOffset);
                currentVertexOffset += vertexCount;

                if (batches.size() > 0)
                {
                    size_t id = batches.size() - 1; // last batch.

                    if (batches.program(id) == program &&
                        batches.texture(id) == texture)
                    {
                        // the batch may continue
                        batches.count(id) += indexCount;
                        return offset;
                    }
                }

                // start a new batch. RenderDefault2D can use any textures for now, as long
                // as they have that little white square in them.
                batches.push(std::move(program),
                             std::move(texture),
                             std::move(istart),
                             std::move(indexCount),
                             {});

                return offset;
            }

            template <typename Vertex_t>
            inline uint16_t addBatch(ShaderProgram *program, Texture *texture, Paint paint, uint32_t vertexCount, uint32_t indexCount, Vertex_t **vout, Index **iout)
            {
                assert(vertexCount >= 3);

                size_t istart = indexBuffer.size();
                size_t iend = istart + indexCount;
                indexBuffer.resize(iend);
                if (iout) *iout = &indexBuffer[istart];

                size_t vstart = vertexBuffer.size();
                size_t vend = vstart + (vertexCount * sizeof(Vertex_t));
                vertexBuffer.resize(vend);
                if (vout) *vout = reinterpret_cast<Vertex_t*>(&vertexBuffer[vstart]);

                uint16_t offset = static_cast<uint16_t>(currentVertexOffset);
                currentVertexOffset += vertexCount;

                if (batches.size() > 0)
                {
                    size_t id = batches.size() - 1; // last batch.

                    if (batches.program(id) == program &&
                        batches.texture(id) == texture &&
                        batches.paint(id) == paint)
                    {
                        // the batch may continue
                        batches.count(id) += indexCount;
                        return offset;
                    }
                }

                batches.push(std::move(program),
                             std::move(texture),
                             std::move(istart),
                             std::move(indexCount),
                             std::move(paint));

                return offset;
            }

            inline void beginFrame(int w, int h, float devicePixelRatio)
            {
                viewWidth = std::move(w);
                viewHeight = std::move(h);
                tessTol = 0.25f / devicePixelRatio;
                distTol = 0.01f / devicePixelRatio;
            }

            inline void endFrame()
            {
                std::function<void(ContextPriv*)> task;
                while (detail::taskQueue.try_dequeue(task))
                {
                    task(this);
                }

                // flush the render Queue.
                if (renderQueue.size() > 0)
                {
                    // Generate Geometry (Multi-threaded)
                    #if defined(_OPENMP)
                    #pragma omp parallel for num_threads(std::thread::hardware_concurrency())
                    #endif
                    for (long i = 0; i < renderQueue.size(); ++i)
                    {
                        #if defined(TUNIS_PROFILING) && defined(_OPENMP)
                        EASY_THREAD_SCOPE("OpenMP trianglation");
                        #endif
                        auto &path = renderQueue.path(i);
                        if (path.dirty())
                        {
                            switch(renderQueue.op(i))
                            {
                                case DRAW_FILL:
                                    generateContour(path);
                                    break;
                                case DRAW_STROKE:
                                    generateStrokeContour(path, renderQueue.state(i));
                                    break;
                            }

                            triangulate(path);

                            path.dirty() = false;
                        }
                    }

                    #if defined(TUNIS_PROFILING)
                    EASY_BLOCK("Batch", profiler::colors::DarkRed);
                    #endif

                    // Batch Geometry into vertex and index buffers
                    for (size_t i = 0; i < renderQueue.size(); ++i)
                    {
                        auto &path = renderQueue.path(i);
                        auto &state = renderQueue.state(i);

                        Paint *paint;
                        switch(renderQueue.op(i))
                        {
                            case DRAW_FILL:
                                paint = &state.fillStyle;
                                break;
                            case DRAW_STROKE:
                                paint = &state.strokeStyle;
                                break;
                        }

                        switch (paint->type())
                        {
                            case PaintType::texture:
                                for(size_t id = 0; id < path.subPathCount(); ++id)
                                {
                                    MPEPolyContext &polyContext = path.subPaths()[id].polyContext;

                                    uint32_t vertexCount = polyContext.PointPoolCount;
                                    uint16_t indexCount = polyContext.TriangleCount*3;

                                    if (vertexCount < 3)
                                    {
                                        continue; // not enough vertices to make a fill. Skip
                                    }

                                    VertexTexture *verticies;
                                    Index *indices;
                                    uint16_t offset;

                                    Color color = paint->colorStops().color(0);
                                    color.a = static_cast<uint8_t>(color.a * state.globalAlpha);

                                    // Do we need to render a shadow?
                                    if (state.shadowColor != Transparent && (glm::epsilonNotEqual(state.shadowOffsetX, 0.0f, glm::epsilon<float>()) || glm::epsilonNotEqual(state.shadowOffsetY, 0.0f, glm::epsilon<float>())))
                                    {
                                        Color shadowColor = state.shadowColor;
                                        shadowColor.a = static_cast<uint8_t>((shadowColor.a/255.0f * color.a/255.0f) * 0xFF);

                                        offset = addBatch(programTexture.get(),
                                                          textures.back().get(),
                                                          vertexCount,
                                                          indexCount,
                                                          &verticies,
                                                          &indices);

                                        //populate the shadow vertices
                                        for (size_t vid = 0; vid < polyContext.PointPoolCount; ++vid)
                                        {
                                            MPEPolyPoint &Point = polyContext.PointsPool[vid];
                                            glm::vec2 pos(Point.X + state.shadowOffsetX,
                                                          Point.Y + state.shadowOffsetY);
                                            glm::vec2 tcoord = pos * gfxStates.pixelWidth;

                                            verticies[vid].a_position = pos;
                                            verticies[vid].a_texcoord.s = static_cast<uint16_t>(tcoord.s);
                                            verticies[vid].a_texcoord.t = static_cast<uint16_t>(tcoord.t);
                                            verticies[vid].a_texoffset.s = static_cast<uint16_t>(0);
                                            verticies[vid].a_texoffset.t = static_cast<uint16_t>(0);
                                            verticies[vid].a_texsize.s = static_cast<uint16_t>(1);
                                            verticies[vid].a_texsize.t = static_cast<uint16_t>(1);
                                            verticies[vid].a_color = shadowColor;
                                        }

                                        //populate the indicies
                                        for (size_t tid = 0; tid < polyContext.TriangleCount; ++tid)
                                        {
                                            MPEPolyTriangle* triangle = polyContext.Triangles[tid];

                                            // get the array index by pointer address arithmetic.
                                            uint16_t p0 = static_cast<uint16_t>(triangle->Points[0] - polyContext.PointsPool);
                                            uint16_t p1 = static_cast<uint16_t>(triangle->Points[1] - polyContext.PointsPool);
                                            uint16_t p2 = static_cast<uint16_t>(triangle->Points[2] - polyContext.PointsPool);

                                            size_t iid = tid * 3;
                                            indices[iid+0] = offset+p2;
                                            indices[iid+1] = offset+p1;
                                            indices[iid+2] = offset+p0;
                                        }

                                    }

                                    offset = addBatch(programTexture.get(),
                                                      textures.back().get(),
                                                      vertexCount,
                                                      indexCount,
                                                      &verticies,
                                                      &indices);

                                    glm::vec2 shapeSize = path.boundBottomRight() - path.boundTopLeft();
                                    glm::vec2 texoffset = glm::vec2(paint->image().bounds().x(), paint->image().bounds().y()) * gfxStates.pixelWidth;
                                    glm::vec2 texsize = glm::vec2(paint->image().bounds().width(), paint->image().bounds().height()) * gfxStates.pixelWidth;
                                    glm::vec2 texscale;

                                    switch (paint->repetition())
                                    {
                                        case RepeatType::repeat:
                                            texscale = glm::vec2(gfxStates.pixelWidth, gfxStates.pixelWidth);
                                            break;
                                        case RepeatType::repeat_x:
                                            texscale.x = gfxStates.pixelWidth;
                                            texscale.y = texsize.y / shapeSize.y;
                                            break;
                                        case RepeatType::repeat_y:
                                            texscale.x = texsize.x / shapeSize.x;
                                            texscale.y = gfxStates.pixelWidth;
                                            break;
                                        case RepeatType::no_repeat:
                                            texscale = texsize / shapeSize;
                                            break;
                                    }

                                    //populate the vertices
                                    for (size_t vid = 0; vid < polyContext.PointPoolCount; ++vid)
                                    {
                                        MPEPolyPoint &Point = polyContext.PointsPool[vid];
                                        glm::vec2 pos(Point.X, Point.Y);
                                        glm::vec2 tcoord = texscale * pos;

                                        verticies[vid].a_position = pos;
                                        verticies[vid].a_texcoord.s = static_cast<uint16_t>(tcoord.s);
                                        verticies[vid].a_texcoord.t = static_cast<uint16_t>(tcoord.t);
                                        verticies[vid].a_texoffset.s = static_cast<uint16_t>(texoffset.s);
                                        verticies[vid].a_texoffset.t = static_cast<uint16_t>(texoffset.t);
                                        verticies[vid].a_texsize.s = static_cast<uint16_t>(texsize.s);
                                        verticies[vid].a_texsize.t = static_cast<uint16_t>(texsize.t);
                                        verticies[vid].a_color = color;
                                    }

                                    //populate the indicies
                                    for (size_t tid = 0; tid < polyContext.TriangleCount; ++tid)
                                    {
                                        MPEPolyTriangle* triangle = polyContext.Triangles[tid];

                                        // get the array index by pointer address arithmetic.
                                        uint16_t p0 = static_cast<uint16_t>(triangle->Points[0] - polyContext.PointsPool);
                                        uint16_t p1 = static_cast<uint16_t>(triangle->Points[1] - polyContext.PointsPool);
                                        uint16_t p2 = static_cast<uint16_t>(triangle->Points[2] - polyContext.PointsPool);

                                        size_t iid = tid * 3;
                                        indices[iid+0] = offset+p2;
                                        indices[iid+1] = offset+p1;
                                        indices[iid+2] = offset+p0;
                                    }
                                }
                                break;
                            case PaintType::gradientLinear:
                                for(size_t id = 0; id < path.subPathCount(); ++id)
                                {
                                    MPEPolyContext &polyContext = path.subPaths()[id].polyContext;

                                    uint32_t vertexCount = polyContext.PointPoolCount;
                                    uint16_t indexCount = polyContext.TriangleCount*3;

                                    if (vertexCount < 3)
                                    {
                                        continue; // not enough vertices to make a fill. Skip
                                    }

                                    VertexGradient *verticies;
                                    Index *indices;
                                    uint16_t offset = addBatch(programGradientLinear.get(),
                                                               textures.back().get(),
                                                               *paint,
                                                               vertexCount,
                                                               indexCount,
                                                               &verticies,
                                                               &indices);

                                    //populate the vertices
                                    for (size_t vid = 0; vid < polyContext.PointPoolCount; ++vid)
                                    {
                                        MPEPolyPoint &Point = polyContext.PointsPool[vid];
                                        verticies[vid].a_position.x = Point.X;
                                        verticies[vid].a_position.y = Point.Y;
                                    }

                                    //populate the indicies
                                    for (size_t tid = 0; tid < polyContext.TriangleCount; ++tid)
                                    {
                                        MPEPolyTriangle* triangle = polyContext.Triangles[tid];

                                        // get the array index by pointer address arithmetic.
                                        uint16_t p0 = static_cast<uint16_t>(triangle->Points[0] - polyContext.PointsPool);
                                        uint16_t p1 = static_cast<uint16_t>(triangle->Points[1] - polyContext.PointsPool);
                                        uint16_t p2 = static_cast<uint16_t>(triangle->Points[2] - polyContext.PointsPool);

                                        size_t iid = tid * 3;
                                        indices[iid+0] = offset+p2;
                                        indices[iid+1] = offset+p1;
                                        indices[iid+2] = offset+p0;
                                    }
                                }
                                break;
                            case PaintType::gradientRadial:
                                for(size_t id = 0; id < path.subPathCount(); ++id)
                                {
                                    MPEPolyContext &polyContext = path.subPaths()[id].polyContext;

                                    uint32_t vertexCount = polyContext.PointPoolCount;
                                    uint16_t indexCount = polyContext.TriangleCount*3;

                                    if (vertexCount < 3)
                                    {
                                        continue; // not enough vertices to make a fill. Skip
                                    }

                                    VertexGradient *verticies;
                                    Index *indices;
                                    uint16_t offset = addBatch(programGradientRadial.get(),
                                                               textures.back().get(),
                                                               *paint,
                                                               vertexCount,
                                                               indexCount,
                                                               &verticies,
                                                               &indices);

                                    //populate the vertices
                                    for (size_t vid = 0; vid < polyContext.PointPoolCount; ++vid)
                                    {
                                        MPEPolyPoint &Point = polyContext.PointsPool[vid];
                                        verticies[vid].a_position.x = Point.X;
                                        verticies[vid].a_position.y = Point.Y;
                                    }

                                    //populate the indicies
                                    for (size_t tid = 0; tid < polyContext.TriangleCount; ++tid)
                                    {
                                        MPEPolyTriangle* triangle = polyContext.Triangles[tid];

                                        // get the array index by pointer address arithmetic.
                                        uint16_t p0 = static_cast<uint16_t>(triangle->Points[0] - polyContext.PointsPool);
                                        uint16_t p1 = static_cast<uint16_t>(triangle->Points[1] - polyContext.PointsPool);
                                        uint16_t p2 = static_cast<uint16_t>(triangle->Points[2] - polyContext.PointsPool);

                                        size_t iid = tid * 3;
                                        indices[iid+0] = offset+p2;
                                        indices[iid+1] = offset+p1;
                                        indices[iid+2] = offset+p0;
                                    }
                                }
                                break;
                        }

                    }

                    renderQueue.resize(0);
                }
                #if defined(TUNIS_PROFILING)
                EASY_END_BLOCK;
                #endif


                #if defined(TUNIS_PROFILING)
                EASY_BLOCK("glBufferData", profiler::colors::DarkRed);
                #endif
                // flush the vertex buffer.
                if (vertexBuffer.size() > 0) {
                    glBufferData(GL_ARRAY_BUFFER,
                                 static_cast<GLsizeiptr>(vertexBuffer.size() * sizeof(VertexTexture)),
                                 vertexBuffer.data(),
                                 GL_STREAM_DRAW);
                    vertexBuffer.resize(0);
                    currentVertexOffset = 0;
                }


                // flush the index buffer.
                if (indexBuffer.size() > 0) {
                    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                                 static_cast<GLsizeiptr>(indexBuffer.size() * sizeof(uint16_t)),
                                 indexBuffer.data(),
                                 GL_STREAM_DRAW);
                    indexBuffer.resize(0);
                }
                #if defined(TUNIS_PROFILING)
                EASY_END_BLOCK;
                #endif

                // flush the batches
                if ( batches.size() > 0)
                {
                    #if defined(TUNIS_PROFILING)
                    EASY_BLOCK("glDrawElements", profiler::colors::DarkRed);
                    #endif
                    for (size_t i = 0; i < batches.size(); ++i)
                    {
                        batches.program(i)->useProgram();
                        batches.program(i)->setViewSizeUniform(viewWidth, viewHeight);

                        const Paint &paint = batches.paint(i);
                        if (paint.type() == detail::PaintType::gradientLinear)
                        {
                            detail::UniformBlock uniforms;

                            glm::vec2 start = paint.start();
                            glm::vec2 end = paint.end();

                            start.y = viewHeight - start.y ;
                            end.y = viewHeight - end.y ;

                            glm::vec2 dt = end - start;

                            uniforms.linearGradient.u_start = start;
                            uniforms.linearGradient.u_dt = dt;
                            uniforms.linearGradient.u_lenSq = glm::dot(dt, dt);

                            size_t colorStopCount = glm::min<size_t>(4, paint.colorStops().size());

                            uniforms.linearGradient.u_colorStopCount = colorStopCount;

                            for (size_t j = 0; j < colorStopCount; ++j)
                            {
                                uniforms.linearGradient.u_offset[j] = paint.colorStops().offset(j);
                                uniforms.linearGradient.u_color[j].r = paint.colorStops().color(j).r / 255.0f;
                                uniforms.linearGradient.u_color[j].g = paint.colorStops().color(j).g / 255.0f;
                                uniforms.linearGradient.u_color[j].b = paint.colorStops().color(j).b / 255.0f;
                                uniforms.linearGradient.u_color[j].a = paint.colorStops().color(j).a / 255.0f;
                            }

                            static_cast<ShaderProgramGradient*>(batches.program(i))->setUniforms(uniforms);
                        }
                        else if (paint.type() == detail::PaintType::gradientRadial)
                        {
                            detail::UniformBlock uniforms;

                            glm::vec2 center = paint.start();
                            glm::vec2 focal = paint.end();

                            center.y = viewHeight - center.y ;
                            focal.y = viewHeight - focal.y ;

                            glm::vec2 dt = focal - center;
                            float dr = paint.radius().x - paint.radius().y;

                            uniforms.radialGradient.u_dt = dt;
                            uniforms.radialGradient.u_focal = focal;
                            uniforms.radialGradient.u_r0 = paint.radius().y;
                            uniforms.radialGradient.u_dr = dr;
                            uniforms.radialGradient.u_a = dt.x * dt.x + dt.y * dt.y - dr * dr;

                            size_t colorStopCount = glm::min<size_t>(4, paint.colorStops().size());

                            uniforms.radialGradient.u_colorStopCount = colorStopCount;

                            for (size_t j = 0; j < colorStopCount; ++j)
                            {
                                uniforms.radialGradient.u_offset[j] = paint.colorStops().offset(j);
                                uniforms.radialGradient.u_color[j].r = paint.colorStops().color(j).r / 255.0f;
                                uniforms.radialGradient.u_color[j].g = paint.colorStops().color(j).g / 255.0f;
                                uniforms.radialGradient.u_color[j].b = paint.colorStops().color(j).b / 255.0f;
                                uniforms.radialGradient.u_color[j].a = paint.colorStops().color(j).a / 255.0f;
                            }

                            static_cast<ShaderProgramGradient*>(batches.program(i))->setUniforms(uniforms);
                        }

                        batches.texture(i)->bind();
                        batches.texture(i)->updateMipmap();


#if 1
                        glDrawElements(GL_TRIANGLES,
                                       static_cast<GLsizei>(batches.count(i)),
                                       GL_UNSIGNED_SHORT,
                                       reinterpret_cast<void*>(batches.offset(i) * sizeof(GLushort)));
#endif

#if 0
                        // Helpful code for debugging triangles.
                        for (size_t j = 0; j < batches.count(i)/3; ++j)
                        {
                            glDrawElements(GL_LINE_LOOP,
                                           3,
                                           GL_UNSIGNED_SHORT,
                                           reinterpret_cast<void*>((batches.offset(i)+(j*3)) * sizeof(GLushort)));
                        }
#endif

#if 0
                        // Helpful code for debugging contours.
                        glDrawElements(GL_LINE_STRIP,
                                       static_cast<GLsizei>(batches.count(i)),
                                       GL_UNSIGNED_SHORT,
                                       reinterpret_cast<void*>(batches.offset(i) * sizeof(GLushort)));
#endif

                    }

                    batches.resize(0);
                }

            }

            inline size_t addSubPath(Path2D &path)
            {
                size_t id = path.subPathCount()++;

                SubPath2D &subPath = path.subPaths()[id];

                subPath.mempool.resize(0);
                subPath.points.resize(0);
                subPath.innerPoints.resize(0);
                subPath.outerPoints.resize(0);
                subPath.closed = false;

                return id;
            }

            inline size_t addSubPath(Path2D &path, glm::vec2 startPos)
            {
                size_t id = addSubPath(path);
                path.subPaths()[id].points.push(std::move(startPos), {}, {}, 0.0f, PointProperties::corner);
                return id;
            }

            inline void addPoint(BorderPointArray &points, glm::vec2 pos, PointProperties = PointProperties::none)
            {
                if (points.size() > 0)
                {
                    if (glm::all(glm::epsilonEqual(points[points.size() - 1], pos, distTol)))
                    {
                        return;
                    }
                }

                points.emplace_back(std::move(pos));
            }

            inline void addPoint(ContourPointArray &points, glm::vec2 pos, PointProperties type)
            {
                if (points.size() > 0)
                {
                    if (glm::all(glm::epsilonEqual(points.pos(points.size() - 1), pos, distTol)))
                    {
                        return;
                    }
                }
                points.push(std::move(pos), {}, {}, 0.0f, std::move(type));
            }


            // based of http://antigrain.com/__code/src/agg_curves.cpp.html by Maxim Shemanarev
            template<typename PointArray>
            inline void bezierTo(PointArray &points,
                                 float x1, float y1,
                                 float x2, float y2,
                                 float x3, float y3,
                                 float x4, float y4)
            {
                recursiveBezier(points, x1, y1, x2, y2, x3, y3, x4, y4, 0);
                addPoint(points, glm::vec2(x4, y4), PointProperties::corner);
            }
            inline float calcSqrtDistance(float x1, float y1, float x2, float y2)
            {
                float dx = x2-x1;
                float dy = y2-y1;
                return dx * dx + dy * dy;
            }
            template<typename PointArray>
            inline void recursiveBezier(PointArray &points,
                                        float x1, float y1,
                                        float x2, float y2,
                                        float x3, float y3,
                                        float x4, float y4,
                                        int32_t level)
            {
                if(level > TUNIS_CURVE_RECURSION_LIMIT)
                {
                    return;
                }

                // Calculate all the mid-points of the line segments
                //----------------------
                float x12   = (x1 + x2) / 2;
                float y12   = (y1 + y2) / 2;
                float x23   = (x2 + x3) / 2;
                float y23   = (y2 + y3) / 2;
                float x34   = (x3 + x4) / 2;
                float y34   = (y3 + y4) / 2;
                float x123  = (x12 + x23) / 2;
                float y123  = (y12 + y23) / 2;
                float x234  = (x23 + x34) / 2;
                float y234  = (y23 + y34) / 2;
                float x1234 = (x123 + x234) / 2;
                float y1234 = (y123 + y234) / 2;

                // Try to approximate the full cubic curve by a single straight line
                //------------------
                float dx = x4-x1;
                float dy = y4-y1;

                float d2 = glm::abs(((x2 - x4) * dy - (y2 - y4) * dx));
                float d3 = glm::abs(((x3 - x4) * dy - (y3 - y4) * dx));

                switch((int(d2 > glm::epsilon<float>()) << 1) + int(d3 > glm::epsilon<float>()))
                {
                    case 0:
                    {
                        // All collinear OR p1==p4
                        //----------------------
                        float k = dx*dx + dy*dy;
                        if(glm::epsilonEqual(k, 0.0f, glm::epsilon<float>()))
                        {
                            d2 = calcSqrtDistance(x1, y1, x2, y2);
                            d3 = calcSqrtDistance(x4, y4, x3, y3);
                        }
                        else
                        {
                            k   = 1 / k;
                            float da1 = x2 - x1;
                            float da2 = y2 - y1;
                            d2  = k * (da1*dx + da2*dy);
                            da1 = x3 - x1;
                            da2 = y3 - y1;
                            d3  = k * (da1*dx + da2*dy);
                            if(d2 > 0 && d2 < 1 && d3 > 0 && d3 < 1)
                            {
                                // Simple collinear case, 1---2---3---4
                                // We can leave just two endpoints
                                return;
                            }
                            if(d2 <= 0) d2 = calcSqrtDistance(x2, y2, x1, y1);
                            else if(d2 >= 1) d2 = calcSqrtDistance(x2, y2, x4, y4);
                            else             d2 = calcSqrtDistance(x2, y2, x1 + d2*dx, y1 + d2*dy);

                            if(d3 <= 0) d3 = calcSqrtDistance(x3, y3, x1, y1);
                            else if(d3 >= 1) d3 = calcSqrtDistance(x3, y3, x4, y4);
                            else             d3 = calcSqrtDistance(x3, y3, x1 + d3*dx, y1 + d3*dy);
                        }
                        if(d2 > d3)
                        {
                            if(d2 < tessTol)
                            {
                                addPoint(points, glm::vec2(x2, y2), PointProperties::none);
                                return;
                            }
                        }
                        else
                        {
                            if(d3 < tessTol)
                            {
                                addPoint(points, glm::vec2(x3, y3), PointProperties::none);
                                return;
                            }
                        }
                        break;
                    }
                    case 1:
                        // p1,p2,p4 are collinear, p3 is significant
                        //----------------------
                        if(d3 * d3 <= tessTol * (dx*dx + dy*dy))
                        {
                            addPoint(points, glm::vec2(x23, y23), PointProperties::none);
                            return;
                        }
                        break;

                    case 2:
                        // p1,p3,p4 are collinear, p2 is significant
                        //----------------------
                        if(d2 * d2 <= tessTol * (dx*dx + dy*dy))
                        {
                            addPoint(points, glm::vec2(x23, y23), PointProperties::none);
                            return;
                        }
                        break;

                    case 3:
                        // Regular case
                        //-----------------
                        if((d2 + d3)*(d2 + d3) <= tessTol * (dx*dx + dy*dy))
                        {
                            addPoint(points, glm::vec2(x23, y23), PointProperties::none);
                            return;
                        }
                        break;
                }

                // Continue subdivision
                //----------------------
                recursiveBezier(points, x1, y1, x12, y12, x123, y123, x1234, y1234, level + 1);
                recursiveBezier(points, x1234, y1234, x234, y234, x34, y34, x4, y4, level + 1);
            }

            template <typename PointArray>
            inline void arc(PointArray &points, glm::vec2 center, float radius,
                            float startAngle, float endAngle, bool anticlockwise)
            {
                float deltaAngle = endAngle - startAngle;

                if (anticlockwise)
                {
                    if (glm::abs(deltaAngle) < glm::two_pi<float>())
                    {
                        while (deltaAngle > 0.0f)
                        {
                            deltaAngle -= glm::two_pi<float>();
                        }
                    }
                    else
                    {
                        deltaAngle = -glm::two_pi<float>();
                    }
                }
                else
                {
                    if (glm::abs(deltaAngle) < glm::two_pi<float>())
                    {
                        while (deltaAngle < 0.0f)
                        {
                            deltaAngle += glm::two_pi<float>();
                        }
                    }
                    else
                    {
                        deltaAngle = glm::two_pi<float>();
                    }
                }

                int32_t segmentCount = static_cast<int32_t>(glm::ceil( glm::abs(deltaAngle) / glm::half_pi<float>()));

                float midAngle = (deltaAngle / segmentCount) * 0.5f;
                float tangentFactor = glm::abs(4.0f / 3.0f * (1.0f - glm::cos(midAngle)) / glm::sin(midAngle));

                if (anticlockwise)
                {
                    tangentFactor = -tangentFactor;
                }

                glm::vec2 dir(glm::cos(startAngle), glm::sin(startAngle));
                glm::vec2 prev = center + dir * radius;
                glm::vec2 prevTan(-dir.y * tangentFactor * radius, dir.x * tangentFactor * radius);
                addPoint(points, prev, PointProperties::corner);

                for (int32_t segment = 1; segment <= segmentCount; ++segment)
                {
                    float angle = startAngle + deltaAngle * (static_cast<float>(segment)/static_cast<float>(segmentCount));
                    dir = glm::vec2(glm::cos(angle), glm::sin(angle));
                    glm::vec2 pos = center + dir * radius;
                    glm::vec2 tan(-dir.y * tangentFactor * radius, dir.x * tangentFactor * radius);

                    bezierTo(points,
                             prev.x, prev.y, // start point
                             prev.x + prevTan.x, prev.y + prevTan.y, // control point 1
                             pos.x - tan.x, pos.y - tan.y, // control point 2
                             pos.x, pos.y); // end point

                    prev = pos;
                    prevTan = tan;
                }
            }

            inline float distPtSeg(const glm::vec2 &c, const glm::vec2 &p, const glm::vec2 &q)
            {
                glm::vec2 pq = q - p;
                glm::vec2 pc = c - p;
                float d = glm::dot(pq, pq);
                float t = glm::dot(pq, pc);

                if (d > 0)
                {
                    t /= d;
                }

                t = glm::clamp(t, 0.0f, 1.0f);

                pc = p + (t*pq) - c;

                return glm::dot(pc, pc);
            }

            template <typename PointArray>
            inline void arcTo(PointArray &points, glm::vec2 p0, glm::vec2 p1, glm::vec2 p2, float radius)
            {
                if (glm::all(glm::epsilonEqual(p0, p1, distTol)))
                {
                    addPoint(points, p1, PointProperties::corner);
                    return;
                }

                if (glm::all(glm::epsilonEqual(p1, p2, distTol)))
                {
                    addPoint(points, p1, PointProperties::corner);
                    return;
                }

                if (distPtSeg(p1, p0, p2) < (distTol * distTol))
                {
                    addPoint(points, p1, PointProperties::corner);
                    return;
                }

                if (radius < distTol)
                {
                    addPoint(points, p1, PointProperties::corner);
                    return;
                }

                glm::vec2 d0 = glm::normalize(p0 - p1);
                glm::vec2 d1 = glm::normalize(p2 - p1);
                float a = glm::acos(glm::dot(d0, d1));
                float d = radius / glm::tan(a * 0.5f);

                if (d > 10000.0f)
                {
                    addPoint(points, p1, PointProperties::corner);
                    return;
                }

                float cp = d1.x * d0.y - d0.x * d1.y;

                glm::vec2 c;
                float a0, a1;
                bool anticlockwise;
                if (cp > 0.0f)
                {
                    c.x = p1.x + d0.x * d + d0.y * radius;
                    c.y = p1.y + d0.y * d + -d0.x * radius;
                    a0 = glm::atan(d0.x, -d0.y);
                    a1 = glm::atan(-d1.x, d1.y);
                    anticlockwise = false;
                }
                else
                {
                    c.x = p1.x + d0.x * d + -d0.y * radius;
                    c.y = p1.y + d0.y * d + d0.x * radius;
                    a0 = glm::atan(-d0.x, d0.y);
                    a1 = glm::atan(d1.x, -d1.y);
                    anticlockwise = true;
                }

                arc(points, c, radius, a0, a1, anticlockwise);
            }


            inline void generateContour(Path2D &path)
            {
                #if defined(TUNIS_PROFILING)
                EASY_FUNCTION(profiler::colors::DarkRed);
                #endif
                SubPath2DArray &subPaths = path.subPaths();
                PathCommandArray &commands = path.commands();

                // reset to default.
                path.subPathCount() = 0;

                size_t id = 0;

                for (size_t i = 0; i < commands.size(); ++i)
                {
                    switch(commands.type(i))
                    {
                        case PathCommandType::close:
                            if (path.subPathCount() > 0)
                            {
                                subPaths[id].closed = true;
                            }
                            break;
                        case PathCommandType::moveTo:
                            id = addSubPath(path, glm::vec2(commands.param0(i), commands.param1(i)));
                            break;
                        case PathCommandType::lineTo:
                            if (path.subPathCount() == 0) { id = addSubPath(path, glm::vec2(0.0f)); }
                            addPoint(subPaths[id].points, glm::vec2(commands.param0(i), commands.param1(i)), PointProperties::corner);
                            break;
                        case PathCommandType::bezierCurveTo:
                        {
                            if (path.subPathCount() == 0) { id = addSubPath(path, glm::vec2(0.0f)); }
                            auto &points = subPaths[id].points;
                            auto &prevPoint = points.pos(points.size()-1);
                            bezierTo(points,
                                     prevPoint.x, prevPoint.y,
                                     commands.param0(i), commands.param1(i),
                                     commands.param2(i), commands.param3(i),
                                     commands.param4(i), commands.param5(i));

                            break;
                        }
                        case PathCommandType::quadraticCurveTo:
                        {
                            if (path.subPathCount() == 0) { id = addSubPath(path, glm::vec2(0.0f)); }
                            auto &points = subPaths[id].points;
                            auto &prevPoint = points.pos(points.size()-1);
                            float x0 = prevPoint.x;
                            float y0 = prevPoint.y;
                            float cx = commands.param0(i);
                            float cy = commands.param1(i);
                            float x = commands.param2(i);
                            float y = commands.param3(i);
                            float c1x = x0 + 2.0f/3.0f*(cx - x0);
                            float c1y = y0 + 2.0f/3.0f*(cy - y0);
                            float c2x = x + 2.0f/3.0f*(cx - x);
                            float c2y = y + 2.0f/3.0f*(cy - y);
                            bezierTo(points, x0, y0, c1x, c1y, c2x, c2y, x, y);
                            break;
                        }
                        case PathCommandType::arc:
                            if (path.subPathCount() == 0) { id = addSubPath(path); }
                            arc(subPaths[id].points,
                                glm::vec2(commands.param0(i),
                                          commands.param1(i)),
                                commands.param2(i),
                                commands.param3(i),
                                commands.param4(i),
                                commands.param5(i) > 0.5f);
                            break;
                        case PathCommandType::arcTo:
                        {
                            if (path.subPathCount() == 0) { id = addSubPath(path, glm::vec2(0.0f)); }
                            auto &points = subPaths[id].points;
                            auto &prevPoint = points.pos(points.size()-1);
                            arcTo(subPaths[id].points,
                                  prevPoint,
                                  glm::vec2(commands.param0(i),
                                            commands.param1(i)),
                                  glm::vec2(commands.param2(i),
                                            commands.param3(i)),
                                  commands.param4(i));
                            break;
                        }
                        case PathCommandType::ellipse:
                            // TODO Ellipse
                            break;
                        case PathCommandType::rect:
                        {
                            float x = commands.param0(i);
                            float y = commands.param1(i);
                            float w = commands.param2(i);
                            float h = commands.param3(i);
                            id = addSubPath(path);
                            auto &points = subPaths[id].points;
                            addPoint(points, glm::vec2(x, y), PointProperties::corner);
                            addPoint(points, glm::vec2(x, y+h), PointProperties::corner);
                            addPoint(points, glm::vec2(x+w, y+h), PointProperties::corner);
                            addPoint(points, glm::vec2(x+w, y), PointProperties::corner);
                            subPaths[id].closed = true;
                            break;
                        }
                    }
                }

                // validate.
                for (size_t id = 0; id < path.subPathCount(); ++id)
                {
                    auto &points = subPaths[id].points;

                    // Check if the first and last point are the same. Get rid of
                    // the last point if that is the case, and close the subpath.
                    if (points.size() >= 2 &&
                        glm::all(glm::epsilonEqual(points.pos(0),
                                                   points.pos(points.size()-1),
                                                   distTol)))
                    {
                        points.resize(points.size()-1);
                        subPaths[id].closed = true;
                    }
                }
            }

            inline void calculateSegmentDirection(Path2D &path)
            {
                // Calculate direction vectors for each points of each subpaths
                for(size_t id = 0; id < path.subPathCount(); ++id)
                {
                    SubPath2D &subPath = path.subPaths()[id];

                    size_t p0, p1;

                    if (subPath.closed)
                    {
                        p0 = subPath.points.size() - 1;
                        p1 = 0;
                    }
                    else
                    {
                        p0 = 0;
                        p1 = 1;
                    }

                    while(p1 < subPath.points.size())
                    {
                        glm::vec2 delta = subPath.points.pos(p1) - subPath.points.pos(p0);
                        float length = glm::length(delta);
                        subPath.points.length(p0) = length;
                        subPath.points.dir(p0) = delta / length;
                        p0 = p1++;
                    }

                    if (!subPath.closed)
                    {
                        // last point should have the same direction than its
                        // previous point.
                        subPath.points.dir(p0) = subPath.points.dir(p0-1);
                    }

                }
            }

            inline void generateStrokeContour(Path2D &path, const ContextState& state)
            {
                generateContour(path);

                #if defined(TUNIS_PROFILING)
                EASY_FUNCTION(profiler::colors::DarkGreen);
                #endif

                float halfLineWidth = state.lineWidth * 0.5f;
                SubPath2DArray &subPaths = path.subPaths();

                calculateSegmentDirection(path);

                // if we have dash lines, we split our subpath into multiple
                // subpaths since linecaps and lineJoin rules apply to
                // every individual dashes.
                if (state.lineDashes.size() > 0)
                {
                    size_t p0, p1;
                    glm::vec2 offset;
                    float currentOffset;
                    size_t id = 0, lineDashId;

                    // TODO figure out a way to do this without causing allocation
                    SubPath2DArray origSubPath = path.subPaths();
                    size_t origSubPathCount = path.subPathCount();
                    path.subPathCount() = 0;

                    for (size_t origId = 0; origId < origSubPathCount; ++origId)
                    {
                        auto &origPoints = origSubPath[origId].points;

                        if (origSubPath[origId].closed)
                        {
                            p0 = origPoints.size() - 1;
                            p1 = 0;
                        }
                        else
                        {
                            p0 = 0;
                            p1 = 1;
                        }

                        while(p1 < origPoints.size())
                        {
                            currentOffset = state.lineDashOffset;

                            lineDashId = 0;

                            // fast foward negative offset to the nearest to zero dash bound.
                            // It should remain negative to be able to create a truncated
                            // starting dash (truncated using glm::clamp below)
                            while (currentOffset + state.lineDashes[lineDashId] <= distTol)
                            {
                                currentOffset += state.lineDashes[lineDashId];
                                if (++lineDashId == state.lineDashes.size())
                                {
                                    lineDashId = 0;
                                }
                            }

                            while (true)
                            {
                                if (currentOffset >= origPoints.length(p0))
                                {
                                    // If this statement is true, this most likely because we reached the end
                                    // of the line while having an unfinished dash line at the end of it.
                                    // We just adding one last point to finish and truncate the final dash.
                                    if (path.subPathCount() > 0 && path.subPaths()[id].points.size() == 1)
                                    {
                                        addPoint(path.subPaths()[id].points, origPoints.pos(p1), PointProperties::corner);
                                    }
                                    break;
                                }

                                offset = origPoints.dir(p0) * glm::clamp(currentOffset, 0.0f, origPoints.length(p0));

                                switch(lineDashId % 2)
                                {
                                    case 0: // Dash Start

                                        id = addSubPath(path, origPoints.pos(p0) + offset);
                                        break;

                                    case 1: // Dash End

                                        // because of the fast forwarding above it's possible to be starting
                                        // on a dash gap. If that is the case, we must not add any points.
                                        if (path.subPathCount() > 0 && path.subPaths()[id].points.size() == 1)
                                        {
                                            addPoint(path.subPaths()[id].points, origPoints.pos(p0) + offset, PointProperties::corner);
                                        }
                                        break;
                                }

                                currentOffset += state.lineDashes[lineDashId];

                                if (++lineDashId == state.lineDashes.size())
                                {
                                    lineDashId = 0;
                                }
                            }

                            p0 = p1++;
                        }
                    }

                    calculateSegmentDirection(path);
                }

                for (size_t id = 0; id < path.subPathCount(); ++id)
                {
                    auto &points = subPaths[id].points;
                    auto &outerPoints = subPaths[id].outerPoints;
                    auto &innerPoints = subPaths[id].innerPoints;

                    if(subPaths[id].closed)
                    {
                        // Calculate normal vectors
                        for (size_t p0 = points.size() - 1, p1 = 0; p1 < points.size(); p0 = p1++)
                        {
                            // rotate direction vector by 90degree CW
                            glm::vec2 dir0 = glm::vec2(points.dir(p0).y, -points.dir(p0).x);
                            glm::vec2 dir1 = glm::vec2(points.dir(p1).y, -points.dir(p1).x);
                            glm::vec2 norm = (dir0 + dir1) * 0.5f;
                            float dot = glm::dot(norm, norm);
                            if (dot > glm::epsilon<float>())
                            {
                                norm *= glm::clamp(1.0f / dot, 0.0f, 1000.0f);
                            }
                            points.norm(p1) = norm;

                            float cross = glm::cross(points.dir(p1), points.dir(p0));
                            points.properties(p1).set(cross > 0.0f ? PointProperties::leftTurn : PointProperties::rightTurn);

                            float sharpnessLimit = glm::min(points.length(p0), points.length(p1)) * (1.0f / halfLineWidth);
                            if (dot * sharpnessLimit * sharpnessLimit > 1.0f)
                            {
                                points.properties(p1).set(PointProperties::sharp);
                            }

                            if (points.properties(p1).test(PointProperties::corner))
                            {
                                if (state.lineJoin == LineJoin::bevel || state.lineJoin == LineJoin::round ||
                                    dot * state.miterLimit * state.miterLimit < 1.0f)
                                {
                                    points.properties(p1).set(PointProperties::bevel);
                                }
                            }
                        }

                        // create inner and outer contour.
                        for (size_t p0 = points.size() - 1, p1 = 0; p1 < points.size(); p0 = p1++)
                        {
                            if (points.properties(p1).test(PointProperties::bevel))
                            {
                                if (state.lineJoin == LineJoin::round)
                                {
                                    glm::vec2 ext0 = glm::vec2(points.dir(p0).y, -points.dir(p0).x) * halfLineWidth;
                                    glm::vec2 ext1 = points.norm(p1) * halfLineWidth;
                                    glm::vec2 ext2 = glm::vec2(points.dir(p1).y, -points.dir(p1).x) * halfLineWidth;

                                    if (points.properties(p1).test(PointProperties::leftTurn))
                                    {
                                        addPoint(innerPoints, points.pos(p1) + ext1);
                                        addPoint(outerPoints, points.pos(p1) - ext0);
                                        arcTo(outerPoints,
                                              points.pos(p1) - ext0,
                                              points.pos(p1) - ext1,
                                              points.pos(p1) - ext2,
                                              halfLineWidth);
                                    }
                                    else
                                    {
                                        addPoint(innerPoints, points.pos(p1) + ext0);
                                        arcTo(innerPoints,
                                              points.pos(p1) + ext0,
                                              points.pos(p1) + ext1,
                                              points.pos(p1) + ext2,
                                              halfLineWidth);
                                        addPoint(outerPoints, points.pos(p1) - ext1);
                                    }
                                }
                                else
                                {
                                    if (points.properties(p1).test(PointProperties::leftTurn))
                                    {
                                        addPoint(innerPoints, points.pos(p1) + points.norm(p1) * halfLineWidth);
                                        addPoint(outerPoints, points.pos(p1) - glm::vec2(points.dir(p0).y, -points.dir(p0).x) * halfLineWidth);
                                        addPoint(outerPoints, points.pos(p1) - glm::vec2(points.dir(p1).y, -points.dir(p1).x) * halfLineWidth);
                                    }
                                    else
                                    {
                                        addPoint(innerPoints, points.pos(p1) + glm::vec2(points.dir(p0).y, -points.dir(p0).x) * halfLineWidth);
                                        addPoint(innerPoints, points.pos(p1) + glm::vec2(points.dir(p1).y, -points.dir(p1).x) * halfLineWidth);
                                        addPoint(outerPoints, points.pos(p1) - points.norm(p1) * halfLineWidth);

                                    }

                                }
                            }
                            else
                            {
                                glm::vec2 extrusion = points.norm(p1) * halfLineWidth;
                                addPoint(innerPoints, points.pos(p1) + extrusion);
                                addPoint(outerPoints, points.pos(p1) - extrusion);
                            }

                        }

                    }
                    else
                    {
                        // Calculate normal vectors
                        points.norm(0) = glm::vec2(points.dir(0).y, -points.dir(0).x); // first point
                        points.norm(points.size()-1) = glm::vec2(points.dir(points.size()-1).y, -points.dir(points.size()-1).x); // last point
                        for (size_t p0 = 0, p1 = 1; p0 < points.size()-2; p0++, p1++)
                        {
                            // rotate direction vector by 90degree CW
                            glm::vec2 dir0 = glm::vec2(points.dir(p0).y, -points.dir(p0).x);
                            glm::vec2 dir1 = glm::vec2(points.dir(p1).y, -points.dir(p1).x);
                            glm::vec2 norm = (dir0 + dir1) * 0.5f;
                            float dot = glm::dot(norm, norm);
                            if (dot > glm::epsilon<float>())
                            {
                                norm *= glm::clamp(1.0f / dot, 0.0f, 1000.0f);
                            }
                            points.norm(p1) = norm;

                            float cross = glm::cross(points.dir(p1), points.dir(p0));
                            points.properties(p1).set(cross > 0.0f ? PointProperties::leftTurn : PointProperties::rightTurn);

                            float sharpnessLimit = glm::max(1.0f, glm::min(points.length(p0), points.length(p1)) * (1.0f / halfLineWidth));
                            if (dot * sharpnessLimit * sharpnessLimit > 1.0f)
                            {
                                points.properties(p1).set(PointProperties::sharp);
                            }

                            if (points.properties(p1).test(PointProperties::corner))
                            {
                                if (state.lineJoin == LineJoin::bevel ||
                                    state.lineJoin == LineJoin::round ||
                                    dot * state.miterLimit * state.miterLimit < 1.0f)
                                {
                                    points.properties(p1).set(PointProperties::bevel);
                                }
                            }
                        }

                        // extrude our points
                        for (size_t p0 = points.size() - 1, p1 = 0; p1 < points.size(); p0 = p1++)
                        {
                            if (points.properties(p1).test(PointProperties::bevel) && points.properties(p1).test(PointProperties::leftTurn))
                            {
                                if (state.lineJoin == LineJoin::round)
                                {
                                    glm::vec2 v0 = points.pos(p1) - glm::vec2(points.dir(p1-1).y, -points.dir(p1-1).x) * halfLineWidth;
                                    glm::vec2 v1 = points.pos(p1) - points.norm(p1) * halfLineWidth;
                                    glm::vec2 v2 = points.pos(p1) - glm::vec2(points.dir(p1).y, -points.dir(p1).x) * halfLineWidth;
                                    addPoint(outerPoints, v0);
                                    arcTo(outerPoints, v0, v1, v2, halfLineWidth);
                                }
                                else
                                {
                                    glm::vec2 v0, v1;
                                    if (points.properties(p1).test(PointProperties::sharp))
                                    {
                                        // rotate direction vectors by 90degree CW
                                        glm::vec2 dir0 = glm::vec2(points.dir(p0).y, -points.dir(p0).x);
                                        glm::vec2 dir1 = glm::vec2(points.dir(p1).y, -points.dir(p1).x);

                                        v0 = points.pos(p1) - dir0 * halfLineWidth;
                                        v1 = points.pos(p1) - dir1 * halfLineWidth;
                                    }
                                    else
                                    {
                                        v0 = points.pos(p1) - points.norm(p0) * halfLineWidth;
                                        v1 = points.pos(p1) - points.norm(p1) * halfLineWidth;
                                    }

                                    addPoint(outerPoints, v0);
                                    addPoint(outerPoints, v1);
                                }
                            }
                            else
                            {
                                addPoint(outerPoints, points.pos(p1) - points.norm(p1) * halfLineWidth);
                            }
                        }

                        // add the end cap
                        if (state.lineCap != LineCap::butt)
                        {
                            glm::vec2 dir = points.dir(points.size()-1) * halfLineWidth;
                            glm::vec2 ext = points.norm(points.size()-1) * halfLineWidth;

                            /*
                              ...>>>>>>>>>(p0)---[+dir]-->(p1)
                              ...-----------------         |
                                                  ---      |
                                                     -- [+ext]
                                                       -   |
                                                        -  V
                                                        - (p2)
                                                        -  |
                                                       -   |
                                                     -- [+ext]
                                                  ---      |
                              ...-----------------         V
                              ...<<<<<<<<<(p4)<--[-dir]---(p3)
                             */

                            const glm::vec2 &p0 = outerPoints[outerPoints.size()-1];
                            glm::vec2 p1 = p0 + dir;
                            glm::vec2 p2 = p1 + ext;
                            glm::vec2 p3 = p2 + ext;
                            glm::vec2 p4 = p3 - dir;

                            if (state.lineCap == LineCap::round)
                            {
                                // then arc 90 degrees from p1 to p3
                                arcTo(outerPoints, p0, p1, p2, halfLineWidth);

                                // then arc 90 degrees from p3 to p5
                                arcTo(outerPoints, p2, p3, p4, halfLineWidth);
                            }
                            else // square
                            {
                                addPoint(outerPoints, p1);
                                addPoint(outerPoints, p2);
                                addPoint(outerPoints, p3);
                                addPoint(outerPoints, p4);
                            }
                        }

                        // extrude the 'other' side of our points in reverse.
                        for (size_t p0 = points.size(), p1 = points.size() - 1; p0 > 0; p0 = p1--)
                        {
                            if (points.properties(p1).test(PointProperties::bevel) && points.properties(p1).test(PointProperties::rightTurn))
                            {
                                if (state.lineJoin == LineJoin::round)
                                {
                                    glm::vec2 v0 = points.pos(p1) + glm::vec2(points.dir(p1).y, -points.dir(p1).x) * halfLineWidth;
                                    glm::vec2 v1 = points.pos(p1) + points.norm(p1) * halfLineWidth;
                                    glm::vec2 v2 = points.pos(p1) + glm::vec2(points.dir(p1+1).y, -points.dir(p1+1).x) * halfLineWidth;
                                    addPoint(outerPoints, v0);
                                    arcTo(outerPoints, v0, v1, v2, halfLineWidth);
                                }
                                else
                                {
                                    glm::vec2 v0, v1;
                                    if (points.properties(p1).test(PointProperties::sharp))
                                    {
                                        // rotate direction vectors by 90degree CW
                                        glm::vec2 dir0 = glm::vec2(points.dir(p0).y, -points.dir(p0).x);
                                        glm::vec2 dir1 = glm::vec2(points.dir(p1).y, -points.dir(p1).x);

                                        v0 = points.pos(p1) + dir0 * halfLineWidth;
                                        v1 = points.pos(p1) + dir1 * halfLineWidth;
                                    }
                                    else
                                    {
                                        v0 = points.pos(p1) + points.norm(p0) * halfLineWidth;
                                        v1 = points.pos(p1) + points.norm(p1) * halfLineWidth;
                                    }

                                    addPoint(outerPoints, v1);
                                    addPoint(outerPoints, v0);
                                }
                            }
                            else
                            {
                                addPoint(outerPoints, points.pos(p1) + points.norm(p1) * halfLineWidth);
                            }
                        }

                        // add the front cap
                        if (state.lineCap != LineCap::butt)
                        {
                            glm::vec2 dir = points.dir(0) * halfLineWidth;
                            glm::vec2 ext = points.norm(0) * halfLineWidth;

                            /*
                                 (p3)---[+dir]-->(p4)>>>>>>>>>...
                                   ^         -----------------...
                                   |      ---
                                [-ext]  --
                                   |   -
                                   |  -
                                 (p2) -
                                   ^  -
                                   |   -
                                [-ext]  --
                                   |      ---
                                   |         -----------------...
                                 (p1)<--[-dir]---(p0)<<<<<<<<<...
                             */

                            const glm::vec2 &p0 = outerPoints[outerPoints.size()-1];
                            glm::vec2 p1 = p0 - dir;
                            glm::vec2 p2 = p1 - ext;
                            glm::vec2 p3 = p2 - ext;
                            glm::vec2 p4 = p3 + dir;

                            if (state.lineCap == LineCap::round)
                            {
                                // arc 90 degrees from p4 to p2
                                arcTo(outerPoints, p0, p1, p2, halfLineWidth);

                                // then arc 90 degrees from p2 to p0
                                arcTo(outerPoints, p2, p3, p4, halfLineWidth);

                                // remove duplicate p4 point since it is already
                                // in outerPoints as the outerPoints[0]
                                outerPoints.resize(outerPoints.size()-1);
                            }
                            else // square
                            {
                                addPoint(outerPoints, p1);
                                addPoint(outerPoints, p2);
                                addPoint(outerPoints, p3);
                                // don't add p4 point since it is already
                                // in outerPoints as the outerPoints[0]
                            }
                        }
                    }
                }
            }

            inline void triangulate(Path2D &path)
            {
                #if defined(TUNIS_PROFILING)
                EASY_FUNCTION(profiler::colors::DarkBlue);
                #endif

                SubPath2DArray &subPaths = path.subPaths();
                glm::vec2 &boundTopLeft = path.boundTopLeft();
                glm::vec2 &boundBottomRight = path.boundBottomRight();
                boundTopLeft = glm::vec2(FLT_MAX);
                boundBottomRight = glm::vec2(-FLT_MAX);

                for (size_t id = 0; id < path.subPathCount(); ++id)
                {
                    MPEPolyContext &polyContext = subPaths[id].polyContext;
                    MemPool &mempool = subPaths[id].mempool;
                    auto &points = subPaths[id].points;
                    auto &innerPoints = subPaths[id].innerPoints;
                    auto &outerPoints = subPaths[id].outerPoints;

                    // The maximum number of points you expect to need
                    // This value is used by the library to calculate
                    // working memory required
                    uint32_t maxPointCount = static_cast<uint32_t>(outerPoints.size() >= 3 ? outerPoints.size() + innerPoints.size() : points.size());

                    // Request how much memory (in bytes) you should
                    // allocate for the library
                    size_t memoryRequired = MPE_PolyMemoryRequired(maxPointCount);

                    // Allocate a memory block of size MemoryRequired
                    // IMPORTANT: The memory must be zero initialized
                    mempool.resize(memoryRequired, 0);

                    // Initialize the poly context by passing the memory pointer,
                    // and max number of points from before
                    MPE_PolyInitContext(&polyContext, mempool.data(), maxPointCount);


                    if (outerPoints.size() >= 3)
                    {
                        // fill outer polypoints buffer.
                        MPEPolyPoint* polyPoints = MPE_PolyPushPointArray(&polyContext, outerPoints.size());
                        for(size_t j = 0; j < outerPoints.size(); ++j)
                        {
                            glm::vec2 &point = outerPoints[j];

                            polyPoints[j].X = point.x;
                            polyPoints[j].Y = point.y;

                            // update path bounds
                            boundTopLeft     = glm::min(boundTopLeft,     point);
                            boundBottomRight = glm::max(boundBottomRight, point);
                        }
                        MPE_PolyAddEdge(&polyContext);

                        if (innerPoints.size() >= 3)
                        {
                            // fill inner polypoints buffer.
                            MPEPolyPoint* polyHoles = MPE_PolyPushPointArray(&polyContext, innerPoints.size());
                            for(size_t j = 0; j < innerPoints.size(); ++j)
                            {
                                glm::vec2 &point = innerPoints[j];

                                polyHoles[j].X = point.x;
                                polyHoles[j].Y = point.y;

                                // update path bounds
                                boundTopLeft     = glm::min(boundTopLeft,     point);
                                boundBottomRight = glm::max(boundBottomRight, point);
                            }
                            MPE_PolyAddHole(&polyContext);
                        }
                    }
                    else if (points.size() >= 3)
                    {
                        MPEPolyPoint* polyPoints = MPE_PolyPushPointArray(&polyContext, points.size());
                        for(size_t j = 0; j < points.size(); ++j)
                        {
                            glm::vec2 &point = points.pos(j);

                            polyPoints[j].X = point.x;
                            polyPoints[j].Y = point.y;

                            // update path bounds
                            boundTopLeft     = glm::min(boundTopLeft,     point);
                            boundBottomRight = glm::max(boundBottomRight, point);
                        }
                        MPE_PolyAddEdge(&polyContext);
                    }

                    MPE_PolyTriangulate(&polyContext);
                }
            }

            inline const Font* findFont(const FontDef &fontDef)
            {
                assert(fontRepo != nullptr);

                const Font *font = nullptr;
                for (flatbuffers::uoffset_t i = 0; i < fontRepo->fonts()->size(); ++i)
                {
                   const Font *candidate = fontRepo->fonts()->Get(i);

                    if (candidate->family()->str() == fontDef.family)
                    {
                        font = candidate;

                        if (candidate->weight() == fontDef.weight && candidate->italic() == fontDef.italic)
                        {
                            break; // perfect candidate!
                        }
                    }
                    else if (!font && candidate->weight() <= fontDef.weight && candidate->italic() == fontDef.italic)
                    {
                        font = candidate;
                    }
                }

                return font;
            }

            inline const Glyph *findGlyph(const Font *font, uint32_t unicode)
            {
                return font->glyphs()->LookupByKey(unicode);
            }

            inline Image getImageForGlyph(const Glyph *glyph)
            {
                auto it = fontGlyphImageCache.find(glyph);
                if (it != fontGlyphImageCache.end())
                {
                    return it->second;
                }

                // TODO create the image and add it to the cache.
                return Image();

            }
        };


    }

    Context::Context() :
        ctx(new detail::ContextPriv())
    {
    }

    Context::~Context()
    {
    }

    const char * Context::backendName() const
    {
        return "GL";
    }

    void Context::clearFrame(int32_t fbLeft, int32_t fbTop, int32_t fbWidth, int32_t fbHeight, Color backgroundColor)
    {
        ctx->clearFrame(std::move(fbLeft), std::move(fbTop),
                        std::move(fbWidth), std::move(fbHeight),
                        std::move(backgroundColor));
    }

    void Context::beginFrame(int32_t winWidth, int32_t winHeight, float devicePixelRatio)
    {
        ctx->beginFrame(std::move(winWidth), std::move(winHeight), std::move(devicePixelRatio));
    }

    void Context::endFrame()
    {
        ctx->endFrame();
    }

    void Context::save()
    {
        ctx->states.push_back(*this);
    }

    void Context::restore()
    {
        if (ctx->states.size() > 0)
        {
            *static_cast<ContextState*>(this) = ctx->states.back();
            ctx->states.pop_back();
        }
    }

    void Context::clearRect(float x, float y, float width, float height)
    {
        Paint origFillStyle = fillStyle;
        fillStyle = detail::gfxStates.backgroundColor;
        rect(x, y, width, height);
        fill();
        fillStyle = origFillStyle;
    }

    void Context::fillText(const char *text, float x, float y, float maxWidth)
    {
        if (ctx->fontRepo == nullptr)
        {
            std::cout << "No font repository loaded. Missing fonts.tfp?" << std::endl;
            return;
        }

        const Font *inst = ctx->findFont(font);
        if (!inst)
        {
            std::cout << "Could not find suitable font candidate for " << font.family << std::endl;
            return;
        }

        for(size_t i = 0; i < strlen(text); ++i)
        {
            const Glyph *glyph = ctx->findGlyph(inst, static_cast<uint32_t>(text[i]));
            if (glyph)
            {
                Image glyphImage = ctx->getImageForGlyph(glyph);
            }
        }
    }

    void Context::strokeText(const char *text, float x, float y, float maxWidth)
    {

    }

    void Context::fill(Path2D &path, FillRule /*fillRule*/)
    {
        ctx->renderQueue.push(detail::DRAW_FILL,
                              path.clone<Path2D>(),
                              std::move(*this));
        path.reset();
    }

    void Context::stroke(Path2D &path)
    {
        ctx->renderQueue.push(detail::DRAW_STROKE,
                              path.clone<Path2D>(),
                              std::move(*this));
        path.reset();
    }

    void Image::sourceChanged(detail::ContextPriv *ctx)
    {
        auto decodeTask = +[](Image *self, std::string url)->void
        {
            #if defined(TUNIS_PROFILING)
            EASY_THREAD_SCOPE(url);
            EASY_FUNCTION();
            #endif

            int w, h, n;
            uint8_t *raw = stbi_load(url.c_str(), &w, &h, &n, 4); // force RGBA

            if (!raw)
            {
                fprintf(stderr, "Could not load %s : %s\n", url.c_str(), stbi_failure_reason());
                return;
            }

            int pw = w + detail::gfxStates.texPadding + detail::gfxStates.texPadding;
            int ph = h + detail::gfxStates.texPadding + detail::gfxStates.texPadding;

            self->data().resize(pw * ph * 4);

            uint8_t *src = raw;
            uint8_t *dst = self->data().data();

            for(int row = 0; row < detail::gfxStates.texPadding; ++row)
            {
                for(size_t col = 0; col < detail::gfxStates.texPadding; ++col)
                {
                    memcpy(dst, &Red, 4);
                    dst+=4;
                }

                memcpy(dst, src, w*4);
                dst+=(w*4);

                for(size_t col = 0; col < detail::gfxStates.texPadding; ++col)
                {
                    memcpy(dst, &Lime, 4);
                    dst+=4;
                }
            }

            for(int row = 0; row < h; ++row)
            {
                for(size_t col = 0; col < detail::gfxStates.texPadding; ++col)
                {
                    memcpy(dst, src, 4);
                    dst+=4;
                }

                memcpy(dst, src, w*4);
                dst+=(w*4);
                src+=((w-1)*4);

                for(size_t col = 0; col < detail::gfxStates.texPadding; ++col)
                {
                    memcpy(dst, src, 4);
                    dst+=4;
                }

                src+=(4);
            }

            src-=(w*4);

            for(int row = 0; row < detail::gfxStates.texPadding; ++row)
            {
                for(size_t col = 0; col < detail::gfxStates.texPadding; ++col)
                {
                    memcpy(dst, &White, 4);
                    dst+=4;
                }

                memcpy(dst, src, w*4);
                dst+=(w*4);

                for(size_t col = 0; col < detail::gfxStates.texPadding; ++col)
                {
                    memcpy(dst, &Blue, 4);
                    dst+=4;
                }
            }

            stbi_image_free(raw);
            self->bounds().setWidth(w);
            self->bounds().setHeight(h);
            self->paddedBounds().setWidth(pw);
            self->paddedBounds().setHeight(ph);
            detail::enqueueTask(&Image::dataChanged, self);
        };

        std::thread(decodeTask, this, source()).detach();
    }

    void Image::dataChanged(detail::ContextPriv *ctx)
    {
        for(size_t i = 0; i < ctx->textures.size(); ++i)
        {
            if (ctx->textures[i]->tryAddImage(*this))
            {
                break;
            }
        }
    }

}
