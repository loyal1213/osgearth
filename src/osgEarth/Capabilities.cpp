/* -*-c++-*- */
/* osgEarth - Geospatial SDK for OpenSceneGraph
 * Copyright 2020 Pelican Mapping
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */
#include <osgEarth/Capabilities>
#include <osgEarth/Version>
#include <osgEarth/SpatialReference>
#include <osgEarth/GEOS>
#include "Registry"
#include <osg/FragmentProgram>
#include <osg/GL2Extensions>
#include <osg/Version>
#include <osgViewer/Version>
#include <gdal.h>
#include <sstream>

using namespace osgEarth;

#define LC "[Capabilities] "

#ifndef GL_CONTEXT_PROFILE_MASK
#define GL_CONTEXT_PROFILE_MASK           0x9126
#endif
#ifndef GL_CONTEXT_CORE_PROFILE_BIT
#define GL_CONTEXT_CORE_PROFILE_BIT       0x00000001
#endif

// ---------------------------------------------------------------------------
// A custom graphics context that we will use to query for OpenGL
// extension and hardware support. (Adapted from osgconv in OpenSceneGraph)

namespace
{
    struct MyGraphicsContext
    {
        MyGraphicsContext()
        {
            // If the number of graphics context is > 0 or < 32 (the default, unitialized value of osg::DisplaySettings::instance()->getMaxNumberOfGraphicsContexts()) then warn users
            // to call osgEarth::initialize before realizing any graphics windows to avoid issues with the maxNumberOfGraphicsContexts being different than the
            // actual number of registered GraphicsContexts in osg.  This can cause issues with unrefAfterApply due to faulty logic in osg::Texture::areAllTextureObjectsLoaded
            // iterating over all of the max number of graphics contexts and checking whether textures are loaded for that context, even if the context isn't still in use.
            // Realizing windows before the capabilities context can also cause odd issues with textures dissapearing in some cases.  It's much safer just to call
            // osgEarth::initialize() at the start of your application to avoid these issues altogether.
            if (osg::DisplaySettings::instance()->getMaxNumberOfGraphicsContexts() > 0 && osg::DisplaySettings::instance()->getMaxNumberOfGraphicsContexts() < 32)
            {
                OE_WARN << "WARNING:  Call osgEarth::initialize() before realizing any graphics windows.  There are currently " << osg::DisplaySettings::instance()->getMaxNumberOfGraphicsContexts() << " graphics contexts." << std::endl;
            }

    	    osg::GraphicsContext::ScreenIdentifier si;
	        si.readDISPLAY();
	        si.setUndefinedScreenDetailsToDefaultScreen();

            osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
    	    traits->hostName = si.hostName;
	        traits->displayNum = si.displayNum;
	        traits->screenNum = si.screenNum;
            traits->x = 0;
            traits->y = 0;
            traits->width = 1;
            traits->height = 1;
            traits->windowDecoration = false;
            traits->doubleBuffer = false;
            traits->sharedContext = 0;
            traits->pbuffer = false;
            traits->glContextVersion = osg::DisplaySettings::instance()->getGLContextVersion();
            traits->glContextProfileMask = osg::DisplaySettings::instance()->getGLContextProfileMask();

            // Intel graphics adapters dont' support pbuffers, and some of their drivers crash when
            // you try to create them. So by default we will only use the unmapped/pbuffer method
            // upon special request.
            if ( getenv( "OSGEARTH_USE_PBUFFER_TEST" ) )
            {
                traits->pbuffer = true;
                OE_INFO << LC << "Activating pbuffer test for graphics capabilities" << std::endl;
                _gc = osg::GraphicsContext::createGraphicsContext(traits.get());
                if ( !_gc.valid() )
                    OE_WARN << LC << "Failed to create pbuffer" << std::endl;
            }

            if (!_gc.valid())
            {
                // fall back on a mapped window
                traits->pbuffer = false;
                _gc = osg::GraphicsContext::createGraphicsContext(traits.get());
            }

            if (_gc.valid())
            {
                _gc->realize();
                _gc->makeCurrent();

                if ( traits->pbuffer == false )
                {
                    OE_DEBUG << LC << "Realized graphics window for OpenGL operations." << std::endl;
                }
                else
                {
                    OE_DEBUG << LC << "Realized pbuffer for OpenGL operations." << std::endl;
                }
            }
            else
            {
                OE_WARN << LC << "Failed to create graphic window too." << std::endl;
            }
        }

        bool valid() const { return _gc.valid() && _gc->isRealized(); }

        osg::ref_ptr<osg::GraphicsContext> _gc;
    };
}

// ---------------------------------------------------------------------------

#define SAYBOOL(X) (X?"yes":"no")

const Capabilities&
Capabilities::get()
{
    return osgEarth::Registry::instance()->capabilities();
}

Capabilities::Capabilities() :
_maxFFPTextureUnits     ( 1 ),
_maxGPUTextureUnits     ( 1 ),
_maxGPUTextureCoordSets ( 1 ),
_maxGPUAttribs          ( 1 ),
_maxTextureSize         ( 256 ),
_maxFastTextureSize     ( 256 ),
_maxLights              ( 1 ),
_depthBits              ( 0 ),
_supportsGLSL           ( false ),
_GLSLversion            ( 1.0f ),
_supportsTextureArrays  ( false ),
_supportsMultiTexture   ( false ),
_supportsStencilWrap    ( true ),
_supportsTwoSidedStencil( false ),
_supportsTexture3D      ( false ),
_supportsTexture2DLod   ( false ),
_supportsMipmappedTextureUpdates( false ),
_supportsDepthPackedStencilBuffer( false ),
_supportsOcclusionQuery ( false ),
_supportsDrawInstanced  ( false ),
_supportsUniformBufferObjects( false ),
_supportsNonPowerOfTwoTextures( false ),
_maxUniformBlockSize    ( 0 ),
_preferDLforStaticGeom  ( true ),
_numProcessors          ( 1 ),
_supportsFragDepthWrite ( false ),
_supportsS3TC           ( false ),
_supportsPVRTC          ( false ),
_supportsARBTC          ( false ),
_supportsETC            ( false ),
_supportsRGTC           ( false ),
_supportsTextureBuffer  ( false ),
_maxTextureBufferSize   ( 0 ),
_isCoreProfile          ( true ),
_supportsVertexArrayObjects ( false ),
_supportsNVGL(false)
{
    // require OSG be built with GL3 support
#ifndef OSG_GL3_AVAILABLE
    OE_WARN << LC << "Warning, OpenSceneGraph does not define OSG_GL3_AVAILABLE; "
        "the application may not function properly" << std::endl;
#endif

    // little hack to force the osgViewer library to link so we can create a graphics context
    osgViewerGetVersion();

    // check the environment in order to disable ATI workarounds
    bool enableATIworkarounds = true;
    if ( ::getenv( "OSGEARTH_DISABLE_ATI_WORKAROUNDS" ) != 0L )
        enableATIworkarounds = false;

    // logical CPUs (cores)
    _numProcessors = std::thread::hardware_concurrency();
    if (_numProcessors <= 0)
        _numProcessors = 4;

    // GLES compile?
#if (defined(OSG_GLES1_AVAILABLE) || defined(OSG_GLES2_AVAILABLE) || defined(OSG_GLES3_AVAILABLE))
    _isGLES = true;
#else
    _isGLES = false;
#endif

    // create a graphics context so we can query OpenGL support:
    osg::GraphicsContext* gc = NULL;
    unsigned int id = 0;
#ifndef __ANDROID__
    MyGraphicsContext mgc;
    if ( mgc.valid() )
    {
        gc = mgc._gc.get();
        id = gc->getState()->getContextID();
    }
#endif

#ifndef __ANDROID__
    if ( gc != NULL )
#endif
    {
        OE_INFO << LC << "Capabilities: " << std::endl;

        const osg::GL2Extensions* GL2 = osg::GL2Extensions::Get( id, true );

        OE_INFO << LC << "  osgEarth Version:  " << osgEarthGetVersion() << std::endl;

#ifdef OSGEARTH_EMBED_GIT_SHA
        OE_INFO << LC << "  osgEarth HEAD SHA: " << osgEarthGitSHA1() << std::endl;
#endif

        OE_INFO << LC << "  OSG Version:       " << osgGetVersion() << std::endl;

#ifdef GDAL_RELEASE_NAME
        OE_INFO << LC << "  GDAL Version:      " << GDAL_RELEASE_NAME << std::endl;
#endif

#ifdef GEOS_VERSION
        OE_INFO << LC << "  GEOS Version:      " << GEOS_VERSION << std::endl;
#endif

        _supportsGLSL = GL2->isGlslSupported;
        _GLSLversion = GL2->glslLanguageVersion;

        _vendor = std::string( reinterpret_cast<const char*>(glGetString(GL_VENDOR)) );
        OE_INFO << LC << "  GPU Vendor:        " << _vendor << std::endl;

        _renderer = std::string( reinterpret_cast<const char*>(glGetString(GL_RENDERER)) );
        OE_INFO << LC << "  GPU Renderer:      " << _renderer << std::endl;

        _version = std::string( reinterpret_cast<const char*>(glGetString(GL_VERSION)) );
        OE_INFO << LC << "  GL/Driver Version: " << _version << 
            " (" << getGLSLVersionInt() << ")" << std::endl;

        // Detect core profile by investigating GL_CONTEXT_PROFILE_MASK
        if ( GL2->glVersion < 3.2f )
        {
            _isCoreProfile = false;
        }
        else
        {
            GLint profileMask = 0;
            glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profileMask);
            _isCoreProfile = ((profileMask & GL_CONTEXT_CORE_PROFILE_BIT) != 0);
        }
        OE_INFO << LC << "  GL Core Profile:   " << SAYBOOL(_isCoreProfile) << std::endl;

        // this extension implies the availability of
        // GL_NV_vertex_buffer_unified_memory (bindless buffers)
        _supportsNVGL =
            GL2->glVersion >= 4.4f &&
            osg::isGLExtensionSupported(id, "GL_NV_vertex_buffer_unified_memory") &&
            osg::isGLExtensionSupported(id, "GL_NV_shader_buffer_load") &&
            osg::isGLExtensionSupported(id, "GL_NV_bindless_multi_draw_indirect");

#if !defined(OSG_GLES2_AVAILABLE) && !defined(OSG_GLES3_AVAILABLE)
        glGetIntegerv( GL_MAX_TEXTURE_UNITS, &_maxFFPTextureUnits );
        //OE_INFO << LC << "  Max FFP texture units = " << _maxFFPTextureUnits << std::endl;
#endif

        glGetIntegerv( GL_MAX_TEXTURE_IMAGE_UNITS_ARB, &_maxGPUTextureUnits );
        OE_DEBUG << LC << "  Max GPU texture units = " << _maxGPUTextureUnits << std::endl;

        GLint mvoc;
        glGetIntegerv(GL_MAX_VERTEX_VARYING_COMPONENTS_EXT, &mvoc);
        OE_DEBUG << LC << "  Max varyings = " << mvoc << std::endl;

#if !defined(OSG_GLES2_AVAILABLE) && !defined(OSG_GLES3_AVAILABLE)
        glGetIntegerv( GL_MAX_TEXTURE_COORDS_ARB, &_maxGPUTextureCoordSets );
#else
        _maxGPUTextureCoordSets = _maxGPUTextureUnits;
#endif
        OE_DEBUG << LC << "  Max GPU texture coord indices = " << _maxGPUTextureCoordSets << std::endl;

        glGetIntegerv( GL_MAX_VERTEX_ATTRIBS, &_maxGPUAttribs );
        OE_DEBUG << LC << "  Max GPU attributes = " << _maxGPUAttribs << std::endl;

#if !(defined(OSG_GL3_AVAILABLE))
        glGetIntegerv( GL_DEPTH_BITS, &_depthBits );
        OE_DEBUG << LC << "  Depth buffer bits = " << _depthBits << std::endl;
#endif

        glGetIntegerv( GL_MAX_TEXTURE_SIZE, &_maxTextureSize );
#if !defined(OSG_GLES1_AVAILABLE) && !defined(OSG_GLES2_AVAILABLE) && !defined(OSG_GLES3_AVAILABLE)
        // Use the texture-proxy method to determine the maximum texture size
        for( int s = _maxTextureSize; s > 2; s >>= 1 )
        {
            glTexImage2D( GL_PROXY_TEXTURE_2D, 0, GL_RGBA8, s, s, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0L );
            GLint width = 0;
            glGetTexLevelParameteriv( GL_PROXY_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width );
            if ( width == s )
            {
                _maxTextureSize = s;
                break;
            }
        }
#endif
        OE_DEBUG << LC << "  Max texture size = " << _maxTextureSize << std::endl;

#ifdef OSG_GL_FIXED_FUNCTION_AVAILABLE
        glGetIntegerv( GL_MAX_LIGHTS, &_maxLights );
#else
        _maxLights = 1;
#endif
        OE_DEBUG << LC << "  GLSL = " << SAYBOOL(_supportsGLSL) << std::endl;

        if ( _supportsGLSL )
        {
            OE_DEBUG << LC << "  GLSL Version = " << getGLSLVersionInt() << std::endl;
        }

        _supportsTextureArrays =
            _supportsGLSL &&
            osg::getGLVersionNumber() >= 2.0f && // hopefully this will detect Intel cards
            osg::isGLExtensionSupported( id, "GL_EXT_texture_array" );
        OE_DEBUG << LC << "  Texture arrays = " << SAYBOOL(_supportsTextureArrays) << std::endl;

        _supportsMultiTexture =
            osg::getGLVersionNumber() >= 1.3f ||
            osg::isGLExtensionSupported( id, "GL_ARB_multitexture") ||
            osg::isGLExtensionSupported( id, "GL_EXT_multitexture" );
        //OE_INFO << LC << "  Multitexturing = " << SAYBOOL(_supportsMultiTexture) << std::endl;

        _supportsStencilWrap = osg::isGLExtensionSupported( id, "GL_EXT_stencil_wrap" );
        //OE_INFO << LC << "  Stencil wrapping = " << SAYBOOL(_supportsStencilWrap) << std::endl;

        _supportsTwoSidedStencil = osg::isGLExtensionSupported( id, "GL_EXT_stencil_two_side" );
        //OE_INFO << LC << "  2-sided stencils = " << SAYBOOL(_supportsTwoSidedStencil) << std::endl;

        _supportsDepthPackedStencilBuffer = osg::isGLExtensionSupported( id, "GL_EXT_packed_depth_stencil" ) ||
                                            osg::isGLExtensionSupported( id, "GL_OES_packed_depth_stencil" );
        //OE_INFO << LC << "  depth-packed stencil = " << SAYBOOL(_supportsDepthPackedStencilBuffer) << std::endl;

        _supportsOcclusionQuery = osg::isGLExtensionSupported( id, "GL_ARB_occlusion_query" );
        //OE_INFO << LC << "  occlusion query = " << SAYBOOL(_supportsOcclusionQuery) << std::endl;

        _supportsDrawInstanced =
            _supportsGLSL &&
            osg::isGLExtensionOrVersionSupported( id, "GL_EXT_draw_instanced", 3.1f );
        OE_DEBUG << LC << "  draw instanced = " << SAYBOOL(_supportsDrawInstanced) << std::endl;

        glGetIntegerv( GL_MAX_UNIFORM_BLOCK_SIZE, &_maxUniformBlockSize );
        //OE_INFO << LC << "  max uniform block size = " << _maxUniformBlockSize << std::endl;

        _supportsUniformBufferObjects =
            _supportsGLSL &&
            osg::isGLExtensionOrVersionSupported( id, "GL_ARB_uniform_buffer_object", 2.0f );
        //OE_INFO << LC << "  uniform buffer objects = " << SAYBOOL(_supportsUniformBufferObjects) << std::endl;

        if ( _supportsUniformBufferObjects && _maxUniformBlockSize == 0 )
        {
            OE_DEBUG << LC << "  ...but disabled, since UBO block size reports zero" << std::endl;
            _supportsUniformBufferObjects = false;
        }

#if !defined(OSG_GLES3_AVAILABLE)
        _supportsNonPowerOfTwoTextures =
            osg::isGLExtensionSupported( id, "GL_ARB_texture_non_power_of_two" );
#else
        _supportsNonPowerOfTwoTextures = true;
#endif
        //OE_INFO << LC << "  NPOT textures = " << SAYBOOL(_supportsNonPowerOfTwoTextures) << std::endl;


#if !defined(OSG_GLES3_AVAILABLE)
        _supportsTextureBuffer =
            osg::isGLExtensionOrVersionSupported( id, "GL_ARB_texture_buffer_object", 3.0 ) ||
            osg::isGLExtensionOrVersionSupported( id, "GL_EXT_texture_buffer_object", 3.0 );
#else
        _supportsTextureBuffer = false;
#endif

        if ( _supportsTextureBuffer )
        {
            glGetIntegerv( GL_MAX_TEXTURE_BUFFER_SIZE, &_maxTextureBufferSize );
        }

        OE_DEBUG << LC << "  Texture buffers = " << SAYBOOL(_supportsTextureBuffer) << std::endl;
        if ( _supportsTextureBuffer )
        {
            OE_DEBUG << LC << "  Texture buffer max size = " << _maxTextureBufferSize << std::endl;
        }

        bool supportsTransformFeedback =
            osg::isGLExtensionSupported( id, "GL_ARB_transform_feedback2" );
        //OE_INFO << LC << "  Transform feedback = " << SAYBOOL(supportsTransformFeedback) << "\n";


        // Writing to gl_FragDepth is not supported under GLES, is supported under gles3
#if (defined(OSG_GLES1_AVAILABLE) || defined(OSG_GLES2_AVAILABLE))
        _supportsFragDepthWrite = false;
#else
        _supportsFragDepthWrite = true;
#endif

        // NVIDIA:
        bool isNVIDIA = _vendor.find("NVIDIA") == 0;

        // ATI workarounds:
        bool isATI = _vendor.find("ATI ") == 0;

        _supportsMipmappedTextureUpdates = isATI && enableATIworkarounds ? false : true;

        _maxFastTextureSize = _maxTextureSize;

        //OE_INFO << LC << "  Max Fast Texture Size = " << _maxFastTextureSize << std::endl;

        // tetxure compression
        std::stringstream buf;
        buf << "  Compression = ";
        _supportsARBTC = osg::isGLExtensionSupported( id, "GL_ARB_texture_compression" );
        if (_supportsARBTC) buf << "ARB ";

        _supportsS3TC = osg::isGLExtensionSupported( id, "GL_EXT_texture_compression_s3tc" );
        if ( _supportsS3TC ) buf << "S3 ";

        _supportsPVRTC = osg::isGLExtensionSupported( id, "GL_IMG_texture_compression_pvrtc" );
        if ( _supportsPVRTC ) buf << "PVR ";

        _supportsETC = osg::isGLExtensionSupported( id, "GL_OES_compressed_ETC1_RGB8_texture" );
        if ( _supportsETC ) buf << "ETC1 ";

        _supportsRGTC = osg::isGLExtensionSupported( id, "GL_EXT_texture_compression_rgtc" );
        if ( _supportsRGTC ) buf << "RG";

        OE_DEBUG << LC << buf.str() << std::endl;

        _supportsVertexArrayObjects = osg::isGLExtensionOrVersionSupported(id, "GL_ARB_vertex_array_object", 3.0);

        _supportsInt64 = osg::isGLExtensionSupported(id, "GL_ARB_gpu_shader_int64");

    }
}

bool
Capabilities::supportsTextureCompression(const osg::Texture::InternalFormatMode& mode) const
{
    switch( mode )
    {
    case osg::Texture::USE_ARB_COMPRESSION:
        return _supportsARBTC;
        break;

    case osg::Texture::USE_S3TC_DXT1a_COMPRESSION:
    case osg::Texture::USE_S3TC_DXT1c_COMPRESSION:
    case osg::Texture::USE_S3TC_DXT1_COMPRESSION:
    case osg::Texture::USE_S3TC_DXT3_COMPRESSION:
    case osg::Texture::USE_S3TC_DXT5_COMPRESSION:
        return _supportsS3TC;
        break;

    case osg::Texture::USE_PVRTC_2BPP_COMPRESSION:
    case osg::Texture::USE_PVRTC_4BPP_COMPRESSION:
        return _supportsPVRTC;
        break;

    case osg::Texture::USE_ETC_COMPRESSION:
        return _supportsETC;
        break;

    case osg::Texture::USE_RGTC1_COMPRESSION:
    case osg::Texture::USE_RGTC2_COMPRESSION:
        return _supportsRGTC;
        break;

    default:
        return false;
    }

    return false;
}
