#include <OpenCL/opencl.h>
#include "ParticleSimulationScene.hpp"

#include "util/paths.hpp"
#include "util/OCL_CALL.hpp"
#include "util/make_unique.hpp"
#include "util/math_util.hpp"
#include "util/cl_util.hpp"

#include "geometry/Primitives.hpp"

namespace pbf {
    using util::make_unique;
    using namespace bwgl;
    using namespace cl;

    ParticleSimulationScene::ParticleSimulationScene(cl::Context &context, cl::Device &device, cl::CommandQueue &queue)
            : BaseScene(context, device, queue) {
        mParticleRadius = 2.0f;

        /// Create shaders
        mParticlesShader = std::make_shared<clgl::BaseShader>(
                std::unordered_map<GLuint, std::string>{{GL_VERTEX_SHADER,   SHADERPATH("particles.vert")},
                                                        {GL_FRAGMENT_SHADER, SHADERPATH("particles.frag")}});
        mParticlesShader->compile();

        mBoxShader = std::make_shared<clgl::BaseShader>(
                std::unordered_map<GLuint, std::string>{{GL_VERTEX_SHADER,   SHADERPATH("box.vert")},
                                                        {GL_FRAGMENT_SHADER, SHADERPATH("box.frag")}});
        mBoxShader->compile();


        /// Create camera
        mCameraRotator = std::make_shared<clgl::SceneObject>();
        mCamera = std::make_shared<clgl::Camera>(glm::uvec2(100, 100), 50);
        clgl::SceneObject::attach(mCameraRotator, mCamera);


        /// Create geometry
        auto boxMesh = clgl::Primitives::createBox(glm::vec3(1.0f, 1.0f, 1.0f));
        boxMesh->flipNormals();
        mBoundingBox = std::make_shared<clgl::MeshObject>(
                std::move(boxMesh),
                mBoxShader
        );

        mBoundsCL = make_unique<pbf::Bounds>();
        mBoundsCL->halfDimensions = {1.0f, 1.0f, 1.0f, 0.0f};
        mBoundsCL->dimensions = {2.0f, 2.0f, 2.0f, 0.0f};

        mGridCL = make_unique<pbf::Grid>();
        mGridCL->halfDimensions = {1.0f, 1.0f, 1.0f, 0.0f};
        mGridCL->binSize = 0.5f;
        mGridCL->binCount3D = {4, 4, 4, 0};
        mGridCL->binCount = 4 * 4 * 4;


        /// Create lights
        mAmbLight = std::make_shared<clgl::AmbientLight>(glm::vec3(0.3f, 0.3f, 1.0f), 0.2f);
        mDirLight = std::make_shared<clgl::DirectionalLight>(glm::vec3(1.0f, 1.0f, 1.0f), 0.1f);
        mPointLight = std::make_shared<clgl::PointLight>(glm::vec3(1.0f, 1.0f, 1.0f), 1.0f);
        mPointLight->setPosition(glm::vec3(0.0f, 2.0f, 0.0f));
        mPointLight->setAttenuation(clgl::Attenuation(0.1f, 0.1f));


        /// Setup particle attributes as OpenGL buffers
        mPositionsGL[0] = make_unique<VertexBuffer>(GL_ARRAY_BUFFER, GL_DYNAMIC_DRAW);
        mPositionsGL[1] = make_unique<VertexBuffer>(GL_ARRAY_BUFFER, GL_DYNAMIC_DRAW);
        mVelocitiesGL[0] = make_unique<VertexBuffer>(GL_ARRAY_BUFFER, GL_DYNAMIC_DRAW);
        mVelocitiesGL[1] = make_unique<VertexBuffer>(GL_ARRAY_BUFFER, GL_DYNAMIC_DRAW);
        mDensitiesGL = make_unique<VertexBuffer>(GL_ARRAY_BUFFER, GL_DYNAMIC_DRAW);
        mParticleBinIDGL = make_unique<VertexBuffer>(GL_ARRAY_BUFFER, GL_DYNAMIC_DRAW);


        /// Create OpenGL vertex array, representing each particle
        mParticles[0] = make_unique<VertexArray>();
        mParticles[0]->bind();
        mParticles[0]->addVertexAttribute(*mPositionsGL[0], 4, GL_FLOAT, GL_FALSE, /*3*sizeof(GLfloat)*/ 0);
        mParticles[0]->addVertexAttribute(*mVelocitiesGL[0], 4, GL_FLOAT, GL_FALSE, /*3*sizeof(GLfloat)*/ 0);
        mParticles[0]->addVertexAttribute(*mDensitiesGL, 1, GL_FLOAT, GL_FALSE, /*sizeof(GLfloat)*/ 0);
            // todo: remove this
            mParticles[0]->addVertexAttribute(*mParticleBinIDGL, 1, GL_UNSIGNED_INT, GL_FALSE, 0);
        mParticles[0]->unbind();

        mParticles[1] = make_unique<VertexArray>();
        mParticles[1]->bind();
        mParticles[1]->addVertexAttribute(*mPositionsGL[1], 4, GL_FLOAT, GL_FALSE, /*3*sizeof(GLfloat)*/ 0);
        mParticles[1]->addVertexAttribute(*mVelocitiesGL[1], 4, GL_FLOAT, GL_FALSE, /*3*sizeof(GLfloat)*/ 0);
        mParticles[1]->addVertexAttribute(*mDensitiesGL, 1, GL_FLOAT, GL_FALSE, /*sizeof(GLfloat)*/ 0);
            // todo: remove this
            mParticles[1]->addVertexAttribute(*mParticleBinIDGL, 1, GL_UNSIGNED_INT, GL_FALSE, 0);
        mParticles[1]->unbind();

        /// Setup counting sort kernels
        std::string kernelSource = "";
        if (TryReadFromFile(KERNELPATH("counting_sort.cl"), kernelSource)) {
            OCL_ERROR;
            std::stringstream ss;
            std::string defines = pbf::getDefinesCL(*mGridCL);
            ss << std::endl << defines << std::endl << kernelSource;
            kernelSource = ss.str();
            OCL_CHECK(mCountingSortProgram = make_unique<Program>(mContext, kernelSource, true, CL_ERROR));
            if (*CL_ERROR == CL_BUILD_PROGRAM_FAILURE) {
                std::cerr << "Error building: "
                          << mCountingSortProgram->getBuildInfo<CL_PROGRAM_BUILD_LOG>(mDevice)
                          << std::endl;
            }

            OCL_CHECK(mSortInsertParticles = make_unique<Kernel>(*mCountingSortProgram, "insert_particles", CL_ERROR));
            OCL_CHECK(mSortComputeBinStartID = make_unique<Kernel>(*mCountingSortProgram, "compute_bin_start_ID", CL_ERROR));
            OCL_CHECK(mSortReindexParticles = make_unique<Kernel>(*mCountingSortProgram, "reindex_particles", CL_ERROR));
        }

        /// Setup timestep kernel
        if (TryReadFromFile(KERNELPATH("timestep.cl"), kernelSource)) {
            cl_int error;
            mTimestepProgram = make_unique<Program>(mContext, kernelSource, true, &error);
            if (error == CL_BUILD_PROGRAM_FAILURE) {
                std::cerr << "Error building: "
                          << mTimestepProgram->getBuildInfo<CL_PROGRAM_BUILD_LOG>(mDevice)
                          << std::endl;
            }

            OCL_ERROR;
            OCL_CHECK(mTimestepKernel = make_unique<Kernel>(*mTimestepProgram, "timestep", CL_ERROR));
        }

        /// Setup "clip to bounds"-kernel
        if (TryReadFromFile(KERNELPATH("clip_to_bounds.cl"), kernelSource)) {
            cl_int error;
            mClipToBoundsProgram = make_unique<Program>(mContext, kernelSource, true, &error);
            if (error == CL_BUILD_PROGRAM_FAILURE) {
                std::cerr << "Error building: "
                          << mClipToBoundsProgram->getBuildInfo<CL_PROGRAM_BUILD_LOG>(mDevice)
                          << std::endl;
            }

            OCL_ERROR;
            OCL_CHECK(mClipToBoundsKernel = make_unique<Kernel>(*mClipToBoundsProgram, "clip_to_bounds", CL_ERROR));
        }

        OCL_CALL(mClipToBoundsKernel->setArg(2, sizeof(pbf::Bounds), mBoundsCL.get()));
    }

    void ParticleSimulationScene::addGUI(nanogui::Screen *screen) {
        auto size = screen->size();

        mCamera->setScreenDimensions(glm::uvec2(size[0], size[1]));
        mCamera->setClipPlanes(0.01f, 100.f);

        using namespace nanogui;
        Window *win = new Window(screen, "Scene Controls");
        win->setPosition(Eigen::Vector2i(15, 125));
        win->setLayout(new GroupLayout());


        /// Fluid scenes
        new Label(win, "Fluid Setups");
        Button *b = new Button(win, "Dam break");
        b->setCallback([&]() {
            this->loadFluidSetup(RESPATH("fluidSetups/dam-break.txt"));
        });
        b = new Button(win, "Cube drop");
        b->setCallback([&]() {
            this->loadFluidSetup(RESPATH("fluidSetups/cube-drop.txt"));
        });


        /// Particles size
        new Label(win, "Particle size");
        Slider *particleSize = new Slider(win);
        particleSize->setCallback([&](float value) {
            mParticleRadius = 20 * value;
        });
        particleSize->setValue(mParticleRadius / 20);


        /// Ambient light parameters

        new Label(win, "Ambient Intensity", "sans", 10);
        Slider *ambIntensity = new Slider(win);
        ambIntensity->setCallback([&](float value){
            mAmbLight->setIntensity(value);
        });
        ambIntensity->setValue(mAmbLight->getIntensity());


        /// Directional light parameters

        new Label(win, "Directional Intensity", "sans", 10);
        Slider *dirIntensity = new Slider(win);
        dirIntensity->setCallback([&](float value){
            mDirLight->setIntensity(value);
        });
        dirIntensity->setValue(mDirLight->getIntensity());


        /// Spot light parameters

        new Label(win, "Point Parameters", "sans", 10);
        new Label(win, "Intensity");
        Slider *spotSlider = new Slider(win);
        spotSlider->setCallback([&](float value){
            mPointLight->setIntensity(value);
        });
        spotSlider->setValue(mPointLight->getIntensity());

        new Label(win, "Attenuation (linear)");
        spotSlider = new Slider(win);
        spotSlider->setCallback([&](float value){
            clgl::Attenuation att = mPointLight->getAttenuation();
            att.a = value * 10;
            mPointLight->setAttenuation(att);
        });
        spotSlider->setValue(mPointLight->getAttenuation().a / 10);

        new Label(win, "Attenuation (quadratic)");
        spotSlider = new Slider(win);
        spotSlider->setCallback([&](float value){
            clgl::Attenuation att = mPointLight->getAttenuation();
            att.b = value * 10;
            mPointLight->setAttenuation(att);
        });
        spotSlider->setValue(mPointLight->getAttenuation().b / 10);
    }

    void ParticleSimulationScene::loadFluidSetup(const std::string &path) {
        std::ifstream ifs(path.c_str());

        const unsigned int MAX_PARTICLES = 10000;
        std::vector<glm::vec4> positions(MAX_PARTICLES);
        std::vector<glm::vec4> velocities(MAX_PARTICLES);
        std::vector<float> densities(MAX_PARTICLES);

        if (ifs.is_open() && !ifs.eof()) {
            ifs >> mNumParticles;

            while (!ifs.eof()) {
                for (unsigned int id = 0; id < mNumParticles; ++id) {
                    ifs >> positions[id].x;
                    ifs >> positions[id].y;
                    ifs >> positions[id].z;
                }
            }
        }

        ifs.close();

        initializeParticleStates(std::move(positions), std::move(velocities), std::move(densities));
    }

    void ParticleSimulationScene::initializeParticleStates(std::vector<glm::vec4> &&positions,
                                                           std::vector<glm::vec4> &&velocities,
                                                           std::vector<float> &&densities) {
        const unsigned int MAX_PARTICLES = positions.size();

        mPositionsGL[0]->bind();
        mPositionsGL[0]->bufferData(4 * sizeof(float) * MAX_PARTICLES, &positions[0]);
        mPositionsGL[0]->unbind();

        mVelocitiesGL[0]->bind();
        mVelocitiesGL[0]->bufferData(4 * sizeof(float) * MAX_PARTICLES, &velocities[0]);
        mVelocitiesGL[0]->unbind();

        mPositionsGL[1]->bind();
        mPositionsGL[1]->bufferData(4 * sizeof(float) * MAX_PARTICLES, &positions[0]);
        mPositionsGL[1]->unbind();

        mVelocitiesGL[1]->bind();
        mVelocitiesGL[1]->bufferData(4 * sizeof(float) * MAX_PARTICLES, &velocities[0]);
        mVelocitiesGL[1]->unbind();

        mDensitiesGL->bind();
        mDensitiesGL->bufferData(sizeof(float) * MAX_PARTICLES, &densities[0]);
        mDensitiesGL->unbind();

        mParticleBinIDGL->bind();
        mParticleBinIDGL->bufferData(sizeof(GLuint) * MAX_PARTICLES, NULL);
        mParticleBinIDGL->unbind();

        /// Create OpenCL references to OpenGL buffers
        OCL_ERROR;
        OCL_CHECK(mPositionsCL[0] = make_unique<BufferGL>(mContext, CL_MEM_READ_WRITE, mPositionsGL[0]->ID(), CL_ERROR));
        mMemObjects.push_back(*mPositionsCL[0]);
        OCL_CHECK(mPositionsCL[1] = make_unique<BufferGL>(mContext, CL_MEM_READ_WRITE, mPositionsGL[1]->ID(), CL_ERROR));
        mMemObjects.push_back(*mPositionsCL[1]);

        OCL_CHECK(mVelocitiesCL[0] = make_unique<BufferGL>(mContext, CL_MEM_READ_WRITE, mVelocitiesGL[0]->ID(), CL_ERROR));
        mMemObjects.push_back(*mVelocitiesCL[0]);
        OCL_CHECK(mVelocitiesCL[1] = make_unique<BufferGL>(mContext, CL_MEM_READ_WRITE, mVelocitiesGL[1]->ID(), CL_ERROR));
        mMemObjects.push_back(*mVelocitiesCL[1]);

        OCL_CHECK(mDensitiesCL = make_unique<BufferGL>(mContext, CL_MEM_READ_WRITE, mDensitiesGL->ID(), CL_ERROR));
        mMemObjects.push_back(*mDensitiesCL);

        OCL_CHECK(mParticleBinIDCL = make_unique<BufferGL>(mContext, CL_MEM_READ_WRITE, mParticleBinIDGL->ID(), CL_ERROR));
        mMemObjects.push_back(*mParticleBinIDCL);

        /// Setup CL-only buffers (for the grid)
        OCL_CHECK(mBinCountCL = make_unique<cl::Buffer>(mContext, CL_MEM_READ_WRITE, sizeof(cl_uint) * mGridCL->binCount, (void*)0, CL_ERROR));
        OCL_CHECK(mBinStartIDCL = make_unique<cl::Buffer>(mContext, CL_MEM_READ_WRITE, sizeof(cl_uint) * mGridCL->binCount, (void*)0, CL_ERROR));
        OCL_CHECK(mParticleInBinPosCL = make_unique<cl::Buffer>(mContext, CL_MEM_READ_WRITE, sizeof(cl_uint) * MAX_PARTICLES, (void*)0, CL_ERROR));

        /// Set these arguments of the kernel since they don't flip their buffers
        OCL_CALL(mSortInsertParticles->setArg(1, *mParticleBinIDCL));
        OCL_CALL(mSortInsertParticles->setArg(2, *mParticleInBinPosCL));
        OCL_CALL(mSortInsertParticles->setArg(3, *mBinCountCL));
        OCL_CALL(mSortComputeBinStartID->setArg(0, *mBinCountCL));
        OCL_CALL(mSortComputeBinStartID->setArg(1, *mBinStartIDCL));
        OCL_CALL(mSortReindexParticles->setArg(0, *mParticleBinIDCL));
        OCL_CALL(mSortReindexParticles->setArg(1, *mParticleInBinPosCL));
        OCL_CALL(mSortReindexParticles->setArg(2, *mBinStartIDCL));

        mCamera->setPosition(glm::vec3(0.0f, 0.0f, 10.0f));
        mDirLight->setLightDirection(glm::vec3(-1.0f));
    }

    void ParticleSimulationScene::reset() {
        const unsigned int MAX_PARTICLES = 1000;
        mNumParticles = 1000;
        mDeltaTime = 1.f / 60;
        mCurrentBufferID = 0;
        mNumSolverIterations = 1;

        std::vector<glm::vec4> positions(MAX_PARTICLES);

        std::vector<float> polarAngles = util::generate_uniform_floats(1000, -CL_M_PI/2, CL_M_PI/2);
        std::vector<float> azimuthalAngles = util::generate_uniform_floats(1000, 0.0f, 2 * CL_M_PI);

        std::vector<glm::vec4> velocities(MAX_PARTICLES);
        {
            glm::vec4 velocity(0);
            float polar, azimuthal;

            for (int i = 0; i < MAX_PARTICLES; ++i) {
                polar = polarAngles[i];
                azimuthal = azimuthalAngles[i];

                velocity.x = sinf(azimuthal) * cosf(polar);
                velocity.y = cosf(azimuthal);
                velocity.z = sinf(azimuthal) * sinf(polar);

                velocities[i] = velocity;
            }
        }

        std::vector<float> densities(MAX_PARTICLES);

        initializeParticleStates(std::move(positions), std::move(velocities), std::move(densities));
    }

    // for all particles i do
    //      apply external forces vi ⇐ vi +∆tfext(xi)
    //      predict position x∗i ⇐ xi +∆tvi
    // end for
    //
    // for all particles i do
    //      find neighboring particles Ni(x∗i)
    // end for
    //
    // while iter < solverIterations do
    //      for all particles i do
    //          calculate λi
    //      end for
    //
    //      for all particles i do
    //          calculate ∆pi
    //          perform collision detection and response
    //      end for
    //
    //      for all particles i do
    //          update position x∗i ⇐ x∗i + ∆pi
    //      end for
    // end while
    //
    // for all particles i do
    //      update velocity vi ⇐ (1/∆t)(x∗i − xi)
    //      apply vorticity confinement and XSPH viscosity
    //      update position xi ⇐ x∗i
    // end for
    void ParticleSimulationScene::update() {
        //unsigned int previousBufferID = mCurrentBufferID;
        unsigned int previousBufferID = 0;
        mCurrentBufferID = 1 - mCurrentBufferID;

        cl::Event event;
        OCL_CALL(mQueue.enqueueAcquireGLObjects(&mMemObjects));

        ///////////////////////////////////////////////////
        /// Apply external forces and predict positions ///
        ///////////////////////////////////////////////////

        OCL_CALL(mTimestepKernel->setArg(0, *mPositionsCL[previousBufferID]));
        OCL_CALL(mTimestepKernel->setArg(1, *mVelocitiesCL[previousBufferID]));
        OCL_CALL(mTimestepKernel->setArg(2, mDeltaTime));
        OCL_CALL(mQueue.enqueueNDRangeKernel(*mTimestepKernel, cl::NullRange,
                                             cl::NDRange(mNumParticles, 1), cl::NullRange));

        /////////////////////
        /// Counting sort ///
        /////////////////////

        /// Reset bin counts to zero, d'uh
        mQueue.enqueueFillBuffer<cl_uint>(*mBinCountCL, 0, 0, sizeof(cl_uint) * mGridCL->binCount);

        OCL_CALL(mSortInsertParticles->setArg(0, *mPositionsCL[previousBufferID]));
        // OCL_CALL(mSortInsertParticles->setArg(1, *mParticleBinIDCL));
        // OCL_CALL(mSortInsertParticles->setArg(2, *mParticleInBinPosCL));
        // OCL_CALL(mSortInsertParticles->setArg(3, *mBinCountCL));
        OCL_CALL(mQueue.enqueueNDRangeKernel(*mSortInsertParticles, cl::NullRange,
                                             cl::NDRange(mNumParticles, 1), cl::NullRange));

        // OCL_CALL(mSortComputeBinStartID->setArg(0, *mBinCountCL));
        // OCL_CALL(mSortComputeBinStartID->setArg(1, *mBinStartIDCL));
        OCL_CALL(mQueue.enqueueNDRangeKernel(*mSortComputeBinStartID, cl::NullRange,
                                             cl::NDRange(mGridCL->binCount, 1), cl::NullRange));

        // OCL_CALL(mSortReindexParticles->setArg(0, *mParticleBinIDCL));
        // OCL_CALL(mSortReindexParticles->setArg(1, *mParticleInBinPosCL));
        // OCL_CALL(mSortReindexParticles->setArg(2, *mBinStartIDCL));
        OCL_CALL(mSortReindexParticles->setArg(3, *mPositionsCL[previousBufferID]));
        OCL_CALL(mSortReindexParticles->setArg(4, *mVelocitiesCL[previousBufferID]));
        OCL_CALL(mSortReindexParticles->setArg(5, *mPositionsCL[mCurrentBufferID]));
        OCL_CALL(mSortReindexParticles->setArg(6, *mVelocitiesCL[mCurrentBufferID]));
        OCL_CALL(mQueue.enqueueNDRangeKernel(*mSortReindexParticles, cl::NullRange,
                                             cl::NDRange(mNumParticles, 1), cl::NullRange));

        //////////////////////////////////
        /// Apply position corrections ///
        //////////////////////////////////

        for (unsigned int i = 0; i < mNumSolverIterations; ++i) {
            ////////////////////
            /// Calculate λi ///
            ////////////////////

            ////////////////////////////////////////////////
            /// calculate ∆pi                            ///
            /// perform collision detection and response ///
            ////////////////////////////////////////////////
            OCL_CALL(mClipToBoundsKernel->setArg(0, *mPositionsCL[mCurrentBufferID]));
            OCL_CALL(mClipToBoundsKernel->setArg(1, *mVelocitiesCL[mCurrentBufferID]));
            //OCL_CALL(mClipToBoundsKernel->setArg(2, sizeof(pbf::Bounds), mBoundsCL.get()));
            OCL_CALL(mQueue.enqueueNDRangeKernel(*mClipToBoundsKernel, cl::NullRange,
                                                 cl::NDRange(mNumParticles, 1), cl::NullRange));

            ////////////////////////////////////////
            /// update position x∗i ⇐ x∗i + ∆pi ///
            ////////////////////////////////////////
        }

        //////////////////////////////////////////////////////
        /// update velocity vi ⇐ (1/∆t)(x∗i − xi)         ///
        /// apply vorticity confinement and XSPH viscosity ///
        /// update position xi ⇐ x∗i                      ///
        //////////////////////////////////////////////////////

        OCL_CALL(mQueue.enqueueReleaseGLObjects(&mMemObjects, NULL, &event));
        OCL_CALL(event.wait());
    }

    void ParticleSimulationScene::render() {
        OGL_CALL(glEnable(GL_DEPTH_TEST));
        OGL_CALL(glEnable(GL_CULL_FACE));
        OGL_CALL(glCullFace(GL_BACK));
        //OGL_CALL(glDisable(GL_PROGRAM_POINT_SIZE));

        const glm::mat4 V = glm::inverse(mCamera->getTransform());
        const glm::mat4 VP = mCamera->getPerspectiveTransform() * V;

        mParticlesShader->use();
        mParticlesShader->uniform("MV", V);
        mParticlesShader->uniform("MVP", VP);
        mParticlesShader->uniform("pointRadius", mParticleRadius);
        mParticlesShader->uniform("pointScale",
                                  mCamera->getScreenDimensions().y / tanf(mCamera->getFieldOfViewY() * CL_M_PI_F / 360));

        mPointLight->setUniformsInShader(mParticlesShader, "pointLight");
        mAmbLight->setUniformsInShader(mParticlesShader, "ambLight");

        //mParticles[mCurrentBufferID]->bind();
        mParticles[0]->bind();
        OGL_CALL(glPointSize(mParticleRadius));
        OGL_CALL(glDrawArrays(GL_POINTS, 0, (GLsizei) mNumParticles));
        //mParticles[mCurrentBufferID]->unbind();
        mParticles[0]->unbind();

        // Cull front faces to only render box insides
        OGL_CALL(glCullFace(GL_FRONT));
        mAmbLight->setUniformsInShader(mBoxShader, "ambLight.");
        mDirLight->setUniformsInShader(mBoxShader, "dirLight.");
        mPointLight->setUniformsInShader(mBoxShader, "pointLight.");
        mBoundingBox->render(VP);
    }

    //////////////////////
    /// INPUT HANDLING ///
    //////////////////////

    bool ParticleSimulationScene::mouseButtonEvent(const glm::ivec2 &p, int button, bool down, int modifiers) {
        if (button == GLFW_MOUSE_BUTTON_LEFT && down) {
            mIsRotatingCamera = true;
        } else if (button == GLFW_MOUSE_BUTTON_LEFT && !down) {
            mIsRotatingCamera = false;
        }

        return false;
    }

    bool
    ParticleSimulationScene::mouseMotionEvent(const glm::ivec2 &p, const glm::ivec2 &rel, int button, int modifiers) {
        if (mIsRotatingCamera) {
            glm::vec3 eulerAngles = mCameraRotator->getEulerAngles();
            eulerAngles.x += 0.05f * rel.y;
            eulerAngles.y += 0.05f * rel.x;
            eulerAngles.x = clamp(eulerAngles.x, - CL_M_PI_F / 2, CL_M_PI_F / 2);
            mCameraRotator->setEulerAngles(eulerAngles);

            return true;
        }

        return false;
    }

    bool ParticleSimulationScene::resizeEvent(const glm::ivec2 &p) {
        mCamera->setScreenDimensions(glm::uvec2(static_cast<unsigned int>(p.x),
                                                static_cast<unsigned int>(p.y)));
        return false;
    }
}