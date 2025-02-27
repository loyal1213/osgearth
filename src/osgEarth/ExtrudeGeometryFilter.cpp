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
#include <osgEarth/ExtrudeGeometryFilter>
#include <osgEarth/Session>
#include <osgEarth/FeatureSourceIndexNode>

#include <osgEarth/ResourceLibrary>
#include <osgEarth/StyleSheet>

#include <osgEarth/ECEF>
#include <osgEarth/ImageUtils>
#include <osgEarth/Clamping>
#include <osgEarth/Utils>
#include <osgEarth/Tessellator>
#include <osgEarth/LineDrawable>
#include <osgEarth/StateSetCache>
#include <osgEarth/Registry>

#include <osg/Geode>
#include <osg/Geometry>
#include <osg/MatrixTransform>
#include <osgUtil/Tessellator>
#include <osgUtil/Optimizer>
#include <osg/LineWidth>
#include <osg/PolygonOffset>

#define LC "[ExtrudeGeometryFilter] "

using namespace osgEarth;

namespace
{
    // Calculates the rotation angle of a shape. This conanically applies to
    // buildings; it finds the longest edge and compares its angle to the
    // x-axis to determine a rotation value. This method is used so we can 
    // properly rotate textures for rooftop application.
    float getApparentRotation( const Geometry* geom )
    {
        Segment n;
        double  maxLen2 = 0.0;
        ConstSegmentIterator i( geom, true );
        while( i.hasMore() )
        {
            Segment s = i.next();
            double len2 = (s.second - s.first).length2();
            if ( len2 > maxLen2 ) 
            {
                maxLen2 = len2;
                n = s;
            }
        }

        const osg::Vec3d& p1 = n.first.x() < n.second.x() ? n.first : n.second;
        const osg::Vec3d& p2 = n.first.x() < n.second.x() ? n.second : n.first;

        return atan2( p2.x()-p1.x(), p2.y()-p1.y() );
    }
}

#define AS_VEC4(V3, X) osg::Vec4f( (V3).x(), (V3).y(), (V3).z(), X )

//------------------------------------------------------------------------

ExtrudeGeometryFilter::ExtrudeGeometryFilter() :
_mergeGeometry         ( true ),
_wallAngleThresh_deg   ( 60.0 ),
_styleDirty            ( true ),
_makeStencilVolume     ( false ),
_gpuClamping           ( false )
{
    _cosWallAngleThresh = cos( _wallAngleThresh_deg );
}

void
ExtrudeGeometryFilter::setStyle( const Style& style )
{
    _style      = style;
    _styleDirty = true;
}

void
ExtrudeGeometryFilter::reset( const FilterContext& context )
{
    _cosWallAngleThresh = cos( _wallAngleThresh_deg );
    _geodes.clear();
    
    if ( _styleDirty )
    {
        const StyleSheet* sheet = context.getSession() ? context.getSession()->styles() : 0L;

        _wallSkinSymbol    = 0L;
        _wallPolygonSymbol = 0L;
        _roofSkinSymbol    = 0L;
        _roofPolygonSymbol = 0L;
        _extrusionSymbol   = 0L;
        _outlineSymbol     = 0L;

        _gpuClamping = false;

        _extrusionSymbol = _style.get<ExtrusionSymbol>();
        if ( _extrusionSymbol.valid() )
        {
            // make a copy of the height expression so we can use it:
            if ( _extrusionSymbol->heightExpression().isSet() )
            {
                _heightExpr = *_extrusionSymbol->heightExpression();
            }

            // If there is no height expression, and we have either absolute or terrain-relative
            // clamping, THAT means that we want to extrude DOWN from the geometry to the ground
            // (instead of from the geometry.)
            AltitudeSymbol* alt = _style.get<AltitudeSymbol>();
            if ( alt && !_extrusionSymbol->heightExpression().isSet() && !_extrusionSymbol->height().isSet() )
            {
                if (alt->clamping() == AltitudeSymbol::CLAMP_ABSOLUTE ||
                    alt->clamping() == AltitudeSymbol::CLAMP_RELATIVE_TO_TERRAIN )
                {
                    _heightExpr = NumericExpression( "0-[__max_hat]" );
                }
            }

            // cache the GPU Clamping directive:
            if ( alt && alt->technique() == AltitudeSymbol::TECHNIQUE_GPU )
            {
                _gpuClamping = true;
            }
            
            // attempt to extract the wall symbols:
            if ( _extrusionSymbol->wallStyleName().isSet() && sheet != 0L )
            {
                const Style* wallStyle = sheet->getStyle( *_extrusionSymbol->wallStyleName(), false );
                if ( wallStyle )
                {
                    _wallSkinSymbol = wallStyle->get<SkinSymbol>();
                    _wallPolygonSymbol = wallStyle->get<PolygonSymbol>();
                }
            }

            // attempt to extract the rooftop symbols:
            if ( _extrusionSymbol->roofStyleName().isSet() && sheet != 0L )
            {
                const Style* roofStyle = sheet->getStyle( *_extrusionSymbol->roofStyleName(), false );
                if ( roofStyle )
                {
                    _roofSkinSymbol = roofStyle->get<SkinSymbol>();
                    _roofPolygonSymbol = roofStyle->get<PolygonSymbol>();
                }
            }

            // if there's a line symbol, use it to outline the extruded data.
            _outlineSymbol = _style.get<LineSymbol>();

            // ...unless a wall poly symbol overrides it.
            if (_wallPolygonSymbol.valid() && _wallPolygonSymbol->outline() == false)
                _outlineSymbol = 0L;

            if (_roofPolygonSymbol.valid() && _roofPolygonSymbol->outline() == false)
                _outlineSymbol = 0L;
        }

        // backup plan for skin symbols:
        const SkinSymbol* skin = _style.get<SkinSymbol>();
        if ( skin )
        {
            if ( !_wallSkinSymbol.valid() )
                _wallSkinSymbol = skin;
            if ( !_roofSkinSymbol.valid() )
                _roofSkinSymbol = skin;
        }

        // backup plan for poly symbols:
        _polySymbol = _style.get<PolygonSymbol>();
        if (_polySymbol.valid())
        {
            if ( !_wallPolygonSymbol.valid() )
                _wallPolygonSymbol = _polySymbol.get();
            if ( !_roofPolygonSymbol.valid() )
                _roofPolygonSymbol = _polySymbol.get();
        }

        _styleDirty = false;
    }
}

bool
ExtrudeGeometryFilter::buildStructure(const Geometry*         input,
                                      double                  height,
                                      bool                    flatten,
                                      float                   verticalOffset,
                                      const SkinResource*     wallSkin,
                                      const SkinResource*     roofSkin,
                                      Structure&              structure,
                                      FilterContext&          cx )
{
    bool makeECEF = false;
    osg::ref_ptr<const SpatialReference> srs;
    osg::ref_ptr<const SpatialReference> mapSRS;

    if ( cx.isGeoreferenced() )
    {
       srs      = cx.extent()->getSRS();
       mapSRS   = cx.getSession()->getMapSRS();
       makeECEF = cx.getSession()->isMapGeocentric();
    }

    // whether this is a closed polygon structure.
    structure.isPolygon = (input->getComponentType() == Geometry::TYPE_POLYGON);

    // store the vert offset for later encoding
    structure.verticalOffset = verticalOffset;

    // extrusion working variables
    double     targetLen = -DBL_MAX;
    osg::Vec3d minLoc(DBL_MAX, DBL_MAX, DBL_MAX);
    double     minLoc_len = DBL_MAX;
    osg::Vec3d maxLoc(0,0,0);
    double     maxLoc_len = 0;

    // Initial pass over the geometry does two things:
    // 1: Calculate the minimum Z across all parts.
    // 2: Establish a "target length" for extrusion
    double absHeight = fabs(height);

    ConstGeometryIterator zfinder( input );
    while( zfinder.hasMore() )
    {
        const Geometry* geom = zfinder.next();
        for( Geometry::const_iterator m = geom->begin(); m != geom->end(); ++m )
        {
            osg::Vec3d m_point = *m;

            if ( m_point.z() + absHeight > targetLen )
                targetLen = m_point.z() + absHeight;

            if (m_point.z() < minLoc.z())
                minLoc = m_point;

            if (m_point.z() > maxLoc.z())
                maxLoc = m_point;
        }
    }

    osg::Vec2d c = input->getBounds().center2d();
    osg::Vec3d centroid(c.x(), c.y(), minLoc.z());

    if (srs.valid() && mapSRS.valid())
    {
        transformAndLocalize(centroid, srs.get(), structure.baseCentroid, mapSRS.get(), _world2local, makeECEF );
    }

    // apply the height offsets
    //height    -= heightOffset;
    //targetLen -= heightOffset;
    
    float   roofRotation  = 0.0f;
    Bounds  roofBounds;
    float   sinR = 0.0f, cosR = 0.0f;
    double  roofTexSpanX = 0.0, roofTexSpanY = 0.0;
    osg::ref_ptr<const SpatialReference> roofProjSRS;

    if ( roofSkin )
    {
        roofBounds = input->getBounds();

        // if our data is lat/long, we need to reproject the geometry and the bounds into a projected
        // coordinate system in order to properly generate tex coords.
        if ( srs && srs->isGeographic() )
        {
            osg::Vec2d geogCenter = roofBounds.center2d();

            // This sometimes fails with the aerodrom stuff. No idea why -gw.
            //roofProjSRS = srs->createUTMFromLonLat( Angle(geogCenter.x()), Angle(geogCenter.y()) );
            roofProjSRS = SpatialReference::create("spherical-mercator");
            if ( roofProjSRS.valid() )
            {
                roofBounds.transform( srs.get(), roofProjSRS.get() );
                osg::ref_ptr<Geometry> projectedInput = input->clone();
                srs->transform( projectedInput->asVector(), roofProjSRS.get() );
                roofRotation = getApparentRotation( projectedInput.get() );
            }
        }
        else
        {
            roofRotation = getApparentRotation( input );
        }
            
        sinR = sin(roofRotation);
        cosR = cos(roofRotation);

        if ( !roofSkin->isTiled().value() )
        {
            //note: non-tiled roofs don't really work atm.
            roofTexSpanX = cosR*roofBounds.width() - sinR*roofBounds.height();
            roofTexSpanY = sinR*roofBounds.width() + cosR*roofBounds.height();
        }
        else
        {
            roofTexSpanX = roofSkin->imageWidth().isSet() ? *roofSkin->imageWidth() : roofSkin->imageHeight().isSet() ? *roofSkin->imageHeight() : 10.0;
            if ( roofTexSpanX <= 0.0 ) roofTexSpanX = 10.0;
            roofTexSpanY = roofSkin->imageHeight().isSet() ? *roofSkin->imageHeight() : roofSkin->imageWidth().isSet() ? *roofSkin->imageWidth() : 10.0;
            if ( roofTexSpanY <= 0.0 ) roofTexSpanY = 10.0;
        }
    }

    // prep for wall texture coordinate generation.
    double texWidthM  = wallSkin ? *wallSkin->imageWidth() : 0.0;
    double texHeightM = wallSkin ? *wallSkin->imageHeight() : 1.0;

    ConstGeometryIterator iter( input );
    while( iter.hasMore() )
    {
        const Geometry* part = iter.next();

        // skip a part that's too small
        if (part->size() < 2)
            continue;

        // add a new wall.
        structure.elevations.push_back(Elevation());
        Elevation& elevation = structure.elevations.back();

        double maxHeight = targetLen - minLoc.z();

        // Adjust the texture height so it is a multiple of the maximum height
        double div = osg::round(maxHeight / texHeightM);
        elevation.texHeightAdjustedM = div > 0.0 ? maxHeight / div : maxHeight;

        // Step 1 - Create the real corners and transform them into our target SRS.
        Corners corners;
        for(Geometry::const_iterator m = part->begin(); m != part->end(); ++m)
        {
            Corners::iterator corner = corners.insert(corners.end(), Corner());
            
            // mark as "from source", as opposed to being inserted by the algorithm.
            corner->isFromSource = true;
            corner->base = *m;

            // extrude:
            if ( height >= 0 ) // extrude up
            {
                if ( flatten )
                    corner->roof.set( corner->base.x(), corner->base.y(), targetLen );
                else
                    corner->roof.set( corner->base.x(), corner->base.y(), corner->base.z() + height );
            }
            else // height < 0 .. extrude down
            {
                corner->roof = *m;
                corner->base.z() += height;
            }
            
            // figure out the rooftop texture coords before doing any transformation:
            if ( roofSkin && srs )
            {
                double xr, yr;

                if ( srs && srs->isGeographic() && roofProjSRS )
                {
                    osg::Vec3d projRoofPt;
                    srs->transform( corner->roof, roofProjSRS.get(), projRoofPt );
                    xr = (projRoofPt.x() - roofBounds.xMin());
                    yr = (projRoofPt.y() - roofBounds.yMin());
                }
                else
                {
                    xr = (corner->roof.x() - roofBounds.xMin());
                    yr = (corner->roof.y() - roofBounds.yMin());
                }

                corner->roofTexU = (cosR*xr - sinR*yr) / roofTexSpanX;
                corner->roofTexV = (sinR*xr + cosR*yr) / roofTexSpanY;
            }

            // transform into target SRS.
            if (srs.valid() && mapSRS.valid())
            {
                transformAndLocalize( corner->base, srs.get(), corner->base, mapSRS.get(), _world2local, makeECEF );
                transformAndLocalize( corner->roof, srs.get(), corner->roof, mapSRS.get(), _world2local, makeECEF );
            }

            // cache the length for later use.
            corner->height = (corner->roof - corner->base).length();
        }

        // Step 2 - Insert intermediate Corners as needed to satify texturing
        // requirements (if necessary) and record each corner offset (horizontal distance
        // from the beginning of the part geometry to the corner.)
        double cornerOffset    = 0.0;
        double nextTexBoundary = texWidthM;

        for(Corners::iterator c = corners.begin(); c != corners.end(); ++c)
        {
            Corners::iterator this_corner = c;

            Corners::iterator next_corner = c;
			bool isLastEdge = false;
			if ( ++next_corner == corners.end() )
			{
				isLastEdge = true;
				next_corner = corners.begin();
			}

            osg::Vec3d base_vec = next_corner->base - this_corner->base;
            double span = base_vec.length();

            this_corner->offsetX = cornerOffset;

            if (wallSkin)
            {
                base_vec /= span; // normalize
                osg::Vec3d roof_vec = next_corner->roof - this_corner->roof;
                roof_vec.normalize();

                while(nextTexBoundary < cornerOffset+span)
                {
                    // insert a new fake corner.
					Corners::iterator new_corner;

                    if (isLastEdge)
                    {
                        if (structure.isPolygon)
                        {
                            corners.push_back(Corner());
                            new_corner = c;
                            new_corner++;
                        }
                        else
                        {
                            // We only add a fake corner at the end for polygons
                            break;
                        }
                    }
                    else
                    {
						new_corner = corners.insert(next_corner, Corner());
					}

                    new_corner->isFromSource = false;
                    double advance = nextTexBoundary-cornerOffset;
                    new_corner->base = this_corner->base + base_vec*advance;
                    new_corner->roof = this_corner->roof + roof_vec*advance;
                    new_corner->height = (new_corner->roof - new_corner->base).length();
                    new_corner->offsetX = cornerOffset + advance;
                    nextTexBoundary += texWidthM;

                    // advance the main iterator
                    c = new_corner;
                }
            }

            cornerOffset += span;
        }

        // Step 3 - Calculate the angle of each corner.
        osg::Vec3d prev_vec;
        for(Corners::iterator c = corners.begin(); c != corners.end(); ++c)
        {
            Corners::const_iterator this_corner = c;

            Corners::const_iterator next_corner = c;
            if ( ++next_corner == corners.end() )
                next_corner = corners.begin();

            if ( this_corner == corners.begin() )
            {
                Corners::const_iterator prev_corner = corners.end();
                --prev_corner;
                prev_vec = this_corner->roof - prev_corner->roof;
                prev_vec.normalize();
            }

            osg::Vec3d this_vec = next_corner->roof - this_corner->roof;
            this_vec.normalize();
            if ( c != corners.begin() )
            {
                c->cosAngle = prev_vec * this_vec;
            }
        }

        // Step 4 - Create faces connecting each pair of Posts.
        Faces& faces = elevation.faces;
        for(Corners::const_iterator c = corners.begin(); c != corners.end(); ++c)
        {
            Corners::const_iterator this_corner = c;

            Corners::const_iterator next_corner = c;
            if ( ++next_corner == corners.end() )
                next_corner = corners.begin();
            
            // only close the shape for polygons.
            if (next_corner != corners.begin() || structure.isPolygon)
            {
                faces.push_back(Face());
                Face& face = faces.back();
                face.left  = *this_corner;
                face.right = *next_corner;

                // recalculate the final offset on the last face
                if ( next_corner == corners.begin() )
                {
                    osg::Vec3d vec = next_corner->roof - this_corner->roof;
                    face.right.offsetX = face.left.offsetX + vec.length();
                }

                face.widthM = next_corner->offsetX - this_corner->offsetX;
            }
        }
    }

    return true;
}

bool
ExtrudeGeometryFilter::buildWallGeometry(const Structure&     structure,
                                         Feature* feature,
                                         osg::Geometry*       walls,
                                         const osg::Vec4&     wallColor,
                                         const osg::Vec4&     wallBaseColor,
                                         const SkinResource*  wallSkin,
                                         FeatureIndexBuilder* index)
{
    bool madeGeom = true;

    // 6 verts per face total (3 triangles)
    unsigned numWallVerts = structure.getNumPoints();

    double texWidthM   = wallSkin ? *wallSkin->imageWidth()  : 1.0;
    double texHeightM  = wallSkin ? *wallSkin->imageHeight() : 1.0;
    bool   useColor    = (!wallSkin || wallSkin->texEnvMode() != osg::TexEnv::DECAL) && !_makeStencilVolume;
    
    // Scale and bias:
    osg::Vec2f scale, bias;
    float layer;
    if ( wallSkin )
    {
        bias.set (wallSkin->imageBiasS().get(),  wallSkin->imageBiasT().get());
        scale.set(wallSkin->imageScaleS().get(), wallSkin->imageScaleT().get());
        layer = (float)wallSkin->imageLayer().get();
    }

    // create all the OSG geometry components
    osg::Vec3Array* verts = static_cast<osg::Vec3Array*>(walls->getVertexArray());
    if (!verts)
    {
        verts = new osg::Vec3Array();
        walls->setVertexArray( verts );
    }
    // Store the current size of the geometry
    unsigned vertptr = verts->size();
    unsigned int startVertPtr = vertptr;

    verts->resize(verts->size() + numWallVerts);

    osg::Vec3Array* tex = 0L;
    if ( wallSkin )
    { 
        tex = static_cast<osg::Vec3Array*>(walls->getTexCoordArray(0));
        if (!tex)
        {
            tex = new osg::Vec3Array();
            walls->setTexCoordArray( 0, tex );
        }
        tex->resize(tex->size() + numWallVerts);
    }

    osg::Vec4Array* colors = 0L;
    if ( useColor )
    {
        colors = static_cast<osg::Vec4Array*>(walls->getColorArray());
        if (!colors)
        {
            colors = new osg::Vec4Array( osg::Array::BIND_PER_VERTEX);
            walls->setColorArray( colors );
        }
        colors->resize(colors->size() + numWallVerts);
    }

    osg::Vec3Array* normals = static_cast<osg::Vec3Array*>(walls->getNormalArray());
    if (!normals)
    {
        normals = new osg::Vec3Array(osg::Array::BIND_PER_VERTEX);
        walls->setNormalArray(normals);
    }
    normals->resize(normals->size() + numWallVerts);
    
    ObjectIDArray* ids = nullptr;
    if (index)
    {
        ids = static_cast<ObjectIDArray*>(walls->getVertexAttribArray(osg::Drawable::SECONDARY_COLORS));
        if (!ids)
        {
            ids = new ObjectIDArray();
            ids->setBinding(osg::Array::BIND_PER_VERTEX);
            ids->setNormalize(false);
            walls->setVertexAttribArray(osg::Drawable::SECONDARY_COLORS, ids);
            ids->setPreserveDataType(true);
        }
        ids->resize(ids->size() + numWallVerts);
    }

    osg::Vec4Array* anchors = 0L;
    
    // If GPU clamping is in effect, create clamping attributes.
    if ( _gpuClamping )
    {     
        anchors = static_cast<osg::Vec4Array*>(walls->getVertexAttribArray(Clamping::AnchorAttrLocation));
        if (!anchors)
        {
            anchors = new osg::Vec4Array( osg::Array::BIND_PER_VERTEX);
            anchors->setNormalize(false);
            walls->setVertexAttribArray( Clamping::AnchorAttrLocation, anchors );
        } 
        anchors->resize(anchors->size() + numWallVerts);
    }

    bool     tex_repeats_y = wallSkin && wallSkin->isTiled() == true;

    bool flatten =
        _style.has<ExtrusionSymbol>() &&
        _style.get<ExtrusionSymbol>()->flatten() == true;

    osg::DrawElements* de = nullptr;
    if (walls->getNumPrimitiveSets() == 0)
    {
        de = new osg::DrawElementsUInt(GL_TRIANGLES);
        walls->addPrimitiveSet(de);
    }
    else
    {
        de = static_cast<osg::DrawElements*>(walls->getPrimitiveSet(0));
    }

    for(Elevations::const_iterator elev = structure.elevations.begin(); elev != structure.elevations.end(); ++elev)
    {
        for(Faces::const_iterator f = elev->faces.begin(); f != elev->faces.end(); ++f, vertptr+=6)
        {
            // set the 6 wall verts.
            (*verts)[vertptr+0] = f->left.roof;
            (*verts)[vertptr+1] = f->left.base;
            (*verts)[vertptr+2] = f->right.base;
            (*verts)[vertptr+3] = f->right.base;
            (*verts)[vertptr+4] = f->right.roof;
            (*verts)[vertptr+5] = f->left.roof;

            const osg::Vec3& v1 = f->left.roof;
            const osg::Vec3& v2 = f->left.base;
            const osg::Vec3& v3 = f->right.base;
            osg::Vec3 normal((v2 - v1) ^ (v3 - v1));
            (*normals)[vertptr + 0] = normal;
            (*normals)[vertptr + 1] = normal;
            (*normals)[vertptr + 2] = normal;
            (*normals)[vertptr + 3] = normal;
            (*normals)[vertptr + 4] = normal;
            (*normals)[vertptr + 5] = normal;
            
            if ( anchors )
            {
                float x = structure.baseCentroid.x(), y = structure.baseCentroid.y(), vo = structure.verticalOffset;

                (*anchors)[vertptr+1].set( x, y, vo, Clamping::ClampToGround );
                (*anchors)[vertptr+2].set( x, y, vo, Clamping::ClampToGround );
                (*anchors)[vertptr+3].set( x, y, vo, Clamping::ClampToGround );

                if ( flatten )
                {
                    (*anchors)[vertptr+0].set( x, y, vo, Clamping::ClampToAnchor );
                    (*anchors)[vertptr+4].set( x, y, vo, Clamping::ClampToAnchor );
                    (*anchors)[vertptr+5].set( x, y, vo, Clamping::ClampToAnchor );
                }
                else
                {                    
                    (*anchors)[vertptr+0].set( x, y, vo + f->left.height,  Clamping::ClampToGround );
                    (*anchors)[vertptr+4].set( x, y, vo + f->right.height, Clamping::ClampToGround );
                    (*anchors)[vertptr+5].set( x, y, vo + f->left.height,  Clamping::ClampToGround );
                }
            }

            // Assign wall polygon colors.
            if (useColor)
            {
                (*colors)[vertptr+0] = wallColor;
                (*colors)[vertptr+1] = wallBaseColor;
                (*colors)[vertptr+2] = wallBaseColor;
                (*colors)[vertptr+3] = wallBaseColor;
                (*colors)[vertptr+4] = wallColor;
                (*colors)[vertptr+5] = wallColor;
            }

            // Calculate texture coordinates:
            if (wallSkin)
            {
                // Calculate left and right corner V coordinates:
                double hL = tex_repeats_y ? (f->left.roof - f->left.base).length()   : elev->texHeightAdjustedM;
                double hR = tex_repeats_y ? (f->right.roof - f->right.base).length() : elev->texHeightAdjustedM;
                
                // Calculate the texture coordinates at each corner. The structure builder
                // will have spaced the verts correctly for this to work.
                float uL = fmod( f->left.offsetX, texWidthM ) / texWidthM;
                float uR = fmod( f->right.offsetX, texWidthM ) / texWidthM;

                // Correct for the case in which the rightmost corner is exactly on a
                // texture boundary.
                if ( uR < uL || (uL == 0.0 && uR == 0.0))
                    uR = 1.0f;

                osg::Vec2f texBaseL( uL, 0.0f );
                osg::Vec2f texBaseR( uR, 0.0f );
                osg::Vec2f texRoofL( uL, hL/elev->texHeightAdjustedM );
                osg::Vec2f texRoofR( uR, hR/elev->texHeightAdjustedM );

                texRoofL = bias + osg::componentMultiply(texRoofL, scale);
                texRoofR = bias + osg::componentMultiply(texRoofR, scale);
                texBaseL = bias + osg::componentMultiply(texBaseL, scale);
                texBaseR = bias + osg::componentMultiply(texBaseR, scale);

                (*tex)[vertptr+0].set( texRoofL.x(), texRoofL.y(), layer );
                (*tex)[vertptr+1].set( texBaseL.x(), texBaseL.y(), layer );
                (*tex)[vertptr+2].set( texBaseR.x(), texBaseR.y(), layer );
                (*tex)[vertptr+3].set( texBaseR.x(), texBaseR.y(), layer );
                (*tex)[vertptr+4].set( texRoofR.x(), texRoofR.y(), layer );
                (*tex)[vertptr+5].set( texRoofL.x(), texRoofL.y(), layer );
            }

            for(int i=0; i<6; ++i)
            {
                de->addElement( vertptr+i );
            }
        }
    }

    if (index)
    {
        index->tagRange(walls, feature, startVertPtr, vertptr - startVertPtr);
    }

    return madeGeom;
}

bool
ExtrudeGeometryFilter::buildRoofGeometry(const Structure&     structure,
                                         Feature* feature,
                                         osg::Geometry*       roof,                                         
                                         const osg::Vec4&     roofColor,
                                         const SkinResource*  roofSkin,
                                         FeatureIndexBuilder* index)
{    
    osg::Vec3Array* verts = static_cast<osg::Vec3Array*>(roof->getVertexArray());
    if (!verts)
    {
        verts = new osg::Vec3Array();
        roof->setVertexArray(verts);
    }
    osg::Vec4Array* color = static_cast<osg::Vec4Array*>(roof->getColorArray());
    if (!color)
    {
        color = new osg::Vec4Array(osg::Array::BIND_PER_VERTEX);
        roof->setColorArray(color);
    }

    osg::Vec3Array* tex = 0L;
    if ( roofSkin )
    {
        tex = static_cast<osg::Vec3Array*>(roof->getTexCoordArray(0));
        if (!tex)
        {
            tex = new osg::Vec3Array();
            roof->setTexCoordArray(0, tex);
        }
    }

    ObjectIDArray* ids = nullptr;
    if (index)
    {
        ids = static_cast<ObjectIDArray*>(roof->getVertexAttribArray(osg::Drawable::SECONDARY_COLORS));
        if (!ids)
        {
            ids = new ObjectIDArray();
            ids->setBinding(osg::Array::BIND_PER_VERTEX);
            ids->setNormalize(false);
            roof->setVertexAttribArray(osg::Drawable::SECONDARY_COLORS, ids);
            ids->setPreserveDataType(true);
        }
    }

    osg::Vec4Array* anchors = 0L;    
    if ( _gpuClamping )
    {
        anchors = static_cast<osg::Vec4Array*>(roof->getVertexAttribArray(Clamping::AnchorAttrLocation));
        if (!anchors)
        {
            anchors = new osg::Vec4Array(osg::Array::BIND_PER_VERTEX);
            anchors->setNormalize(false);
            roof->setVertexAttribArray(Clamping::AnchorAttrLocation, anchors);
        }
    }

    osg::Vec3Array* normal = static_cast<osg::Vec3Array*>(roof->getNormalArray());
    if (!normal)
    {
        normal = new osg::Vec3Array(osg::Array::BIND_PER_VERTEX);
        roof->setNormalArray(normal);
    }

    bool flatten =
        _style.has<ExtrusionSymbol>() &&
        _style.get<ExtrusionSymbol>()->flatten() == true;


    osg::ref_ptr< osg::Geometry > tempGeom = new osg::Geometry;
    osg::Vec3Array* tempVerts = new osg::Vec3Array;
    tempGeom->setVertexArray(tempVerts);

    // Create a series of line loops that the tessellator can reorganize
    // into polygons.
    unsigned int vertptr = 0;// verts->size();
    unsigned int startVertPtr = verts->size();
    for(Elevations::const_iterator e = structure.elevations.begin(); e != structure.elevations.end(); ++e)
    {
        unsigned elevptr = vertptr;
        for(Faces::const_iterator f = e->faces.begin(); f != e->faces.end(); ++f)
        {
            // Only use source verts; we skip interim verts inserted by the 
            // structure building since they are co-linear anyway and thus we don't
            // need them for the roof line.
            if ( f->left.isFromSource )
            {
                verts->push_back( f->left.roof );
                tempVerts->push_back(f->left.roof);
                color->push_back( roofColor );
                normal->push_back(osg::Vec3(0, 0, 1));

                if ( tex )
                {
                    tex->push_back( osg::Vec3f(f->left.roofTexU, f->left.roofTexV, (float)0.0f) );
                }

                if ( anchors )
                {
                    float 
                        x = structure.baseCentroid.x(),
                        y = structure.baseCentroid.y(), 
                        vo = structure.verticalOffset;

                    if ( flatten )
                    {
                        anchors->push_back( osg::Vec4f(x, y, vo, Clamping::ClampToAnchor) );
                    }
                    else
                    {
                        anchors->push_back( osg::Vec4f(x, y, vo + f->left.height, Clamping::ClampToGround) );
                    }
                }
                ++vertptr;
            }
        }
        tempGeom->addPrimitiveSet( new osg::DrawArrays(GL_LINE_LOOP, elevptr, vertptr-elevptr) );
    } 

    // Tessellate the roof lines into polygons.
    osgEarth::Tessellator oeTess;
    if (!oeTess.tessellateGeometry(*tempGeom))
    {
        //fallback to osg tessellator
        OE_DEBUG << LC << "Falling back on OSG tessellator (" << roof->getName() << ")" << std::endl;

        osgUtil::Tessellator tess;
        tess.setTessellationType( osgUtil::Tessellator::TESS_TYPE_GEOMETRY );
        tess.setWindingType( osgUtil::Tessellator::TESS_WINDING_ODD );
        tess.retessellatePolygons( *tempGeom);
    }

    // Get or create the primitive set
    osg::DrawElementsUInt* de = nullptr;
    if (roof->getNumPrimitiveSets() == 0)
    {
        de = new osg::DrawElementsUInt(GL_TRIANGLES);
        roof->addPrimitiveSet(de);
    }
    else
    {
        de = static_cast<osg::DrawElementsUInt*>(roof->getPrimitiveSet(0));
    }

    // Add the tesselated polygon to the main DrawElements, offseting the indices since the tesselation is going to 
    // return values based a zero index.  This might be something we need to address later.
    for (unsigned int i = 0; i < tempGeom->getNumPrimitiveSets(); ++i)
    {
        osg::DrawElementsUInt* p = static_cast<osg::DrawElementsUInt*>(tempGeom->getPrimitiveSet(i));
        if (p)
        {
            for (unsigned int j = 0; j < p->size(); ++j)
            {
                de->addElement(p->at(j) + startVertPtr);
            }
        }        
    }    

    if (index)
    {
        unsigned count = vertptr;
        ids->resize(ids->size() + count);
        index->tagRange(roof, feature, startVertPtr, count);
    }

    return true;
}

osg::Drawable*
ExtrudeGeometryFilter::buildOutlineGeometry(const Structure& structure)
{
    // minimum angle between adjacent faces for which to draw a post.
    const float cosMinAngle = cos(osg::DegreesToRadians(_outlineSymbol->creaseAngle().get()));

    osg::ref_ptr<LineDrawable> lines = new LineDrawable(GL_LINES);

    // if the user requested legacy lines:
    if (_outlineSymbol->useGLLines() == true)
        lines->setUseGPU(false);
    
    const optional<Stroke>& stroke = _outlineSymbol->stroke();
    if (stroke.isSet())
    {
        lines->setColor(stroke->color());

        if (stroke->width().isSet())
            lines->setLineWidth(stroke->width().get());

        if (stroke->stipplePattern().isSet())
            lines->setStipplePattern(stroke->stipplePattern().get());

        if (stroke->stippleFactor().isSet())
            lines->setStippleFactor(stroke->stippleFactor().get());
    }

    osg::Vec4Array* anchors = 0L;
    if ( _gpuClamping )
    {
        anchors = new osg::Vec4Array();
        anchors->setBinding(osg::Array::BIND_PER_VERTEX);
        lines->setVertexAttribArray( Clamping::AnchorAttrLocation, anchors );
    }

    bool flatten =
        _style.has<ExtrusionSymbol>() &&
        _style.get<ExtrusionSymbol>()->flatten() == true;
    
    float
        x  = structure.baseCentroid.x(),
        y  = structure.baseCentroid.y(),
        vo = structure.verticalOffset;

    for(Elevations::const_iterator e = structure.elevations.begin(); e != structure.elevations.end(); ++e)
    {
        osg::Vec3d prev_vec;

        for(Faces::const_iterator f = e->faces.begin(); f != e->faces.end(); ++f)
        {
            // Only use source verts for posts.
            bool drawPost     = f->left.isFromSource;
            bool drawCrossbar = true;

            osg::Vec3d this_vec = f->right.roof - f->left.roof;
            this_vec.normalize();

            if (f->left.isFromSource && f != e->faces.begin())
            {
                drawPost = (this_vec * prev_vec) < cosMinAngle;
            }

            if (drawPost)
            {
                lines->pushVertex(f->left.roof);
                lines->pushVertex(f->left.base);
                
                if (anchors)
                {
                    if (flatten) 
                    {
                        lines->pushVertexAttrib(anchors, osg::Vec4f(x, y, vo, Clamping::ClampToAnchor));
                        lines->pushVertexAttrib(anchors, osg::Vec4f(x, y, vo, Clamping::ClampToAnchor));
                    }
                    else
                    {
                        lines->pushVertexAttrib(anchors, osg::Vec4f(x, y, vo + f->left.height, Clamping::ClampToGround));
                        lines->pushVertexAttrib(anchors, osg::Vec4f(x, y, vo, Clamping::ClampToGround));
                    }
                }
            }

            if (drawCrossbar)
            {
                lines->pushVertex(f->left.roof);
                lines->pushVertex(f->right.roof);
                
                if (anchors)
                {
                    if (flatten) 
                    {
                        lines->pushVertexAttrib(anchors, osg::Vec4f(x, y, vo, Clamping::ClampToAnchor));
                        lines->pushVertexAttrib(anchors, osg::Vec4f(x, y, vo, Clamping::ClampToAnchor));
                    }
                    else
                    {
                        lines->pushVertexAttrib(anchors, osg::Vec4f(x, y, vo + f->left.height, Clamping::ClampToGround));
                        lines->pushVertexAttrib(anchors, osg::Vec4f(x, y, vo + f->right.height, Clamping::ClampToGround));
                    }
                }
            }

            prev_vec = this_vec;
        }

        // Draw an end-post if this isn't a closed polygon.
        if ( !structure.isPolygon )
        {
            Faces::const_iterator last = e->faces.end()-1;

            lines->pushVertex(last->right.roof);

            if (anchors)
            {
                if (flatten)
                    lines->pushVertexAttrib(anchors, osg::Vec4f(x, y, vo, Clamping::ClampToAnchor));
                else
                    lines->pushVertexAttrib(anchors, osg::Vec4f(x, y, vo + last->right.height, Clamping::ClampToGround));
            }

            lines->pushVertex(last->right.base);

            if (anchors)
                lines->pushVertexAttrib(anchors, osg::Vec4f(x, y, vo, Clamping::ClampToGround));
        }
    }

    // finalize the line set
    lines->dirty();

    return lines->empty() ? 0L : lines.release();
}

void
ExtrudeGeometryFilter::addDrawable(osg::Drawable*       drawable,
                                   osg::StateSet*       stateSet,
                                   const std::string&   name,
                                   Feature*             feature,
                                   FeatureIndexBuilder* index )
{
    // find the geode for the active stateset, creating a new one if necessary. NULL is a 
    // valid key as well.
    osg::Group* geode;
    
    if (dynamic_cast<LineDrawable*>(drawable))
    {
        geode = _lineGroups[stateSet].get();
        if (!geode)
        {
            geode = new LineGroup();
            if (stateSet)
            {
                geode->getOrCreateStateSet()->merge(*stateSet);
            }
            _lineGroups[stateSet] = geode;
        }
    }
    else
    {
        geode = _geodes[stateSet].get();
        if (!geode)
        {
            geode = new osg::Geode();
            geode->setStateSet(stateSet);
            _geodes[stateSet] = geode;
        }
    }

    geode->addChild( drawable );
}

bool
ExtrudeGeometryFilter::process( FeatureList& features, FilterContext& context )
{
    for( FeatureList::iterator f = features.begin(); f != features.end(); ++f )
    {
        Feature* input = f->get();

        // run a symbol script if present.
        if (_polySymbol.valid() && _polySymbol->script().isSet())
        {
            StringExpression temp(_polySymbol->script().get());
            input->eval(temp, &context);
        }

        if (input->getGeometry() == 0L)
            continue;

        // run a symbol script if present.
        if ( _extrusionSymbol->script().isSet() )
        {
            StringExpression temp( _extrusionSymbol->script().get() );
            input->eval( temp, &context );
        }

        if (input->getGeometry() == 0L)
            continue;

        // iterator over the parts.
        GeometryIterator iter( input->getGeometry(), false );
        while( iter.hasMore() )
        {
            Geometry* part = iter.next();

            // calculate the extrusion height:
            float height;

            if (_heightCallback.valid())
            {
                height = _heightCallback->operator()(input, context);
            }
            else if (_heightExpr.isSet())
            {
                height = input->eval(_heightExpr.mutable_value(), &context);
            }
            else
            {
                height = *_extrusionSymbol->height();
            }

            // Set up for feature naming and feature indexing:
            std::string name;
            if (!_featureNameExpr.empty())
                name = input->eval(_featureNameExpr, &context);

            osg::ref_ptr<osg::StateSet> wallStateSet;
            osg::ref_ptr<osg::StateSet> roofStateSet;

            // calculate the wall texturing:
            SkinResource* wallSkin = 0L;
            if (_wallSkinSymbol.valid())
            {
                unsigned int wallRand = f->get()->getFID() + (_wallSkinSymbol.valid() ? *_wallSkinSymbol->randomSeed() : 0);

                if (_wallResLib.valid())
                {
                    SkinSymbol querySymbol(*_wallSkinSymbol.get());
                    querySymbol.objectHeight() = fabs(height);
                    wallSkin = _wallResLib->getSkin(&querySymbol, wallRand, context.getDBOptions());
                }

                else
                {
                    // nop
                }

                if (wallSkin)
                {
                    context.resourceCache()->getOrCreateStateSet(wallSkin, wallStateSet, context.getDBOptions());
                }
            }

            // calculate the rooftop texture:
            SkinResource* roofSkin = 0L;
            if (_roofSkinSymbol.valid())
            {
                unsigned int roofRand = f->get()->getFID() + (_roofSkinSymbol.valid() ? *_roofSkinSymbol->randomSeed() : 0);

                if (_roofResLib.valid())
                {
                    SkinSymbol querySymbol(*_roofSkinSymbol.get());
                    roofSkin = _roofResLib->getSkin(&querySymbol, roofRand, context.getDBOptions());
                }

                else
                {
                    // nop
                }

                if (roofSkin)
                {
                    // Get a stateset for the individual roof skin
                    context.resourceCache()->getOrCreateStateSet(roofSkin, roofStateSet, context.getDBOptions());
                }
            }

            osg::ref_ptr<osg::Geometry> walls = _wallGeometries[wallStateSet.get()];
            if (!walls.valid())
            {
                walls = new osg::Geometry();
                walls->setName("Walls");
                walls->setUseVertexBufferObjects(true);
                _wallGeometries[wallStateSet.get()] = walls.get();
                addDrawable(walls.get(), wallStateSet.get(), name, input, context.featureIndex());                
            }

            osg::ref_ptr<osg::Geometry> rooflines = 0L;
            osg::ref_ptr<osg::Geometry> baselines = 0L;
            osg::ref_ptr<osg::Drawable> outlines  = 0L;

            if (part->getType() == Geometry::TYPE_POLYGON)
            {
                part->rewind(osgEarth::Geometry::ORIENTATION_CCW);

                rooflines = _roofGeometries[roofStateSet.get()];
                if (!rooflines.valid())
                {
                    rooflines = new osg::Geometry();
                    rooflines->setName("Roofs");
                    rooflines->setUseVertexBufferObjects(true);
                    _roofGeometries[roofStateSet.get()] = rooflines.get();
                    addDrawable(rooflines.get(), roofStateSet.get(), name, input, context.featureIndex());
                }

                // prep the shapes by making sure all polys are open:
                static_cast<Polygon*>(part)->open();
            }

            // make a base cap if we're doing stencil volumes.
            if ( _makeStencilVolume )
            {
                baselines = _baselineGeometries[nullptr];
                if (!baselines.valid())
                {
                    baselines = new osg::Geometry();
                    baselines->setName(typeid(*this).name());
                    baselines->setUseVertexBufferObjects(true);
                    _baselineGeometries[nullptr] = baselines.get();
                    addDrawable(baselines.get(), 0L, name, input, context.featureIndex());
                }
            }

            float verticalOffset = (float)input->getDouble("__oe_verticalOffset", 0.0);

            // Build the data model for the structure.
            Structure structure;

            buildStructure(
                part, 
                height,
                _extrusionSymbol->flatten().get(),
                verticalOffset,
                wallSkin,
                roofSkin,
                structure,
                context);

            // Create the walls.
            if ( walls.valid() )
            {
                osg::Vec4f wallColor(1,1,1,1), wallBaseColor(1,1,1,1);

                if ( _wallPolygonSymbol.valid() )
                {
                    wallColor = _wallPolygonSymbol->fill()->color();
                }

                if ( _extrusionSymbol->wallGradientPercentage().isSet() )
                {
                    wallBaseColor = Color(wallColor).brightness( 1.0 - *_extrusionSymbol->wallGradientPercentage() );
                }
                else
                {
                    wallBaseColor = wallColor;
                }

                buildWallGeometry(structure, input, walls.get(), wallColor, wallBaseColor, wallSkin, context.featureIndex());
            }

            // tessellate and add the roofs if necessary:
            if ( rooflines.valid() )
            {
                osg::Vec4f roofColor(1,1,1,1);
                if ( _roofPolygonSymbol.valid() )
                {
                    roofColor = _roofPolygonSymbol->fill()->color();
                }
                buildRoofGeometry(structure, input, rooflines.get(), roofColor, roofSkin, context.featureIndex());                
            }

            if (_outlineSymbol.valid())
            {
                outlines = buildOutlineGeometry(structure);
                addDrawable(outlines.get(), 0L, name, input, context.featureIndex());
            }

            if ( baselines.valid() )
            {
                osgUtil::Tessellator tess;
                tess.setTessellationType( osgUtil::Tessellator::TESS_TYPE_GEOMETRY );
                tess.setWindingType( osgUtil::Tessellator::TESS_WINDING_ODD );
                tess.retessellatePolygons( *(baselines.get()) );
            }        
        }
    }

    return true;
}

osg::Node*
ExtrudeGeometryFilter::push( FeatureList& input, FilterContext& context )
{
    reset( context );

    // minimally, we require an extrusion symbol.
    if ( !_extrusionSymbol.valid() )
    {
        OE_WARN << LC << "Missing required extrusion symbolology; geometry will be empty" << std::endl;
        return new osg::Group();
    }

    // establish the active resource library, if applicable.
    _wallResLib = 0L;
    _roofResLib = 0L;

    const StyleSheet* sheet = context.getSession() ? context.getSession()->styles() : 0L;

    if ( sheet != 0L )
    {
        if ( _wallSkinSymbol.valid() && _wallSkinSymbol->library().isSet() )
        {
            _wallResLib = sheet->getResourceLibrary( *_wallSkinSymbol->library() );

            if ( !_wallResLib.valid() )
            {
                OE_WARN << LC << "Unable to load resource library '" << *_wallSkinSymbol->libraryName() << "'"
                    << "; wall geometry will not be textured." << std::endl;
                _wallSkinSymbol = 0L;
            }
        }

        if ( _roofSkinSymbol.valid() && _roofSkinSymbol->library().isSet() )
        {
            _roofResLib = sheet->getResourceLibrary( *_roofSkinSymbol->library() );
            if ( !_roofResLib.valid() )
            {
                OE_WARN << LC << "Unable to load resource library '" << *_roofSkinSymbol->library() << "'"
                    << "; roof geometry will not be textured." << std::endl;
                _roofSkinSymbol = 0L;
            }
        }
    }

    // calculate the localization matrices (_local2world and _world2local)
    computeLocalizers( context );

    // push all the features through the extruder.
    bool ok = process( input, context );

    // parent geometry with a delocalizer (if necessary)
    osg::Group* group = createDelocalizeGroup();

    unsigned int numDrawables = 0;
    for( SortedGeodeMap::iterator i = _geodes.begin(); i != _geodes.end(); ++i )
    {
        group->addChild( i->second.get() );
        numDrawables += i->second->getNumChildren();
    }
    _geodes.clear();

    for (SortedGeodeMap::iterator i = _lineGroups.begin(); i != _lineGroups.end(); ++i)
    {
        group->addChild(i->second.get());
    }
    _lineGroups.clear();

    // Prepare buffer objects.
    AllocateAndMergeBufferObjectsVisitor allocAndMerge;
    group->accept( allocAndMerge );

    // set a uniform indicating that clamping attributes are available.
    Clamping::installHasAttrsUniform( group->getOrCreateStateSet() );

    // if we drew outlines, apply a poly offset too.
    if ( _outlineSymbol.valid() )
    {
        osg::StateSet* groupStateSet = group->getOrCreateStateSet();
        groupStateSet->setAttributeAndModes( new osg::PolygonOffset(1,1), 1 );
#ifdef OSG_GL_FIXED_FUNCTION_AVAILABLE
        if ( _outlineSymbol->stroke()->width().isSet() )
            groupStateSet->setAttributeAndModes( new osg::LineWidth(*_outlineSymbol->stroke()->width()), 1 );
#endif
    }

    return group;
}
