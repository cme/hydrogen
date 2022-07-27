/*
 * Hydrogen
 * Copyright(c) 2002-2008 by Alex >Comix< Cominu [comix@users.sourceforge.net]
 * Copyright(c) 2008-2021 The hydrogen development team [hydrogen-devel@lists.sourceforge.net]
 *
 * http://www.hydrogen-music.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY, without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see https://www.gnu.org/licenses
 *
 */

#include <core/Basics/InstrumentLayer.h>

#include <core/Helpers/Filesystem.h>
#include <core/Helpers/Xml.h>
#include <core/Basics/Sample.h>
#include <core/License.h>
#include <core/Preferences/Preferences.h>

namespace H2Core
{

InstrumentLayer::InstrumentLayer( std::shared_ptr<Sample> sample ) :
	__start_velocity( 0.0 ),
	__end_velocity( 1.0 ),
	__pitch( 0.0 ),
	__gain( 1.0 ),
	__sample( sample )
{
}

InstrumentLayer::InstrumentLayer( std::shared_ptr<InstrumentLayer> other ) : Object( *other ),
	__start_velocity( other->get_start_velocity() ),
	__end_velocity( other->get_end_velocity() ),
	__pitch( other->get_pitch() ),
	__gain( other->get_gain() ),
	__sample( other->get_sample() )
{
}

InstrumentLayer::InstrumentLayer( std::shared_ptr<InstrumentLayer> other, std::shared_ptr<Sample> sample ) : Object( *other ),
	__start_velocity( other->get_start_velocity() ),
	__end_velocity( other->get_end_velocity() ),
	__pitch( other->get_pitch() ),
	__gain( other->get_gain() ),
	__sample( sample )
{
}

InstrumentLayer::~InstrumentLayer()
{
}

void InstrumentLayer::set_sample( std::shared_ptr<Sample> sample )
{
	__sample = sample;
}

void InstrumentLayer::load_sample( float fBpm )
{
	if ( __sample != nullptr ) {
		__sample->load( fBpm );
	}
}

void InstrumentLayer::unload_sample()
{
	if ( __sample != nullptr ) {
		__sample->unload();
	}
}

std::shared_ptr<InstrumentLayer> InstrumentLayer::load_from( XMLNode* pNode, const QString& sDrumkitPath, const License& drumkitLicense, bool bSilent )
{
	QString sFilename = pNode->read_string( "filename", "", false, false, bSilent );
	if ( ! Filesystem::file_exists( sFilename, true ) && ! sDrumkitPath.isEmpty() &&
		 ! sFilename.startsWith( "/" ) ) {
		sFilename = sDrumkitPath + "/" + sFilename;
	}

	std::shared_ptr<Sample> pSample = nullptr;
	if ( Filesystem::file_exists( sFilename, true ) ) {
		pSample = std::make_shared<Sample>( sFilename, drumkitLicense );

		// If 'ismodified' is not present, InstrumentLayer was stored as
		// part of a drumkit. All the additional Sample info, like Loops,
		// envelopes etc., were not written to disk and we won't load the
		// sample.
		bool bIsModified = pNode->read_bool( "ismodified", false, true, false, true );
		pSample->set_is_modified( bIsModified );
	
		if ( bIsModified ) {
		
			Sample::Loops loops;
			loops.mode = Sample::parse_loop_mode( pNode->read_string( "smode", "forward", false, false, bSilent ) );
			loops.start_frame = pNode->read_int( "startframe", 0, false, false, bSilent );
			loops.loop_frame = pNode->read_int( "loopframe", 0, false, false, bSilent );
			loops.count = pNode->read_int( "loops", 0, false, false, bSilent );
			loops.end_frame = pNode->read_int( "endframe", 0, false, false, bSilent );
			pSample->set_loops( loops );
	
			Sample::Rubberband rubberband;
			rubberband.use = pNode->read_int( "userubber", 0, false, false, bSilent );
			rubberband.divider = pNode->read_float( "rubberdivider", 0.0, false, false, bSilent );
			rubberband.c_settings = pNode->read_int( "rubberCsettings", 1, false, false, bSilent );
			rubberband.pitch = pNode->read_float( "rubberPitch", 0.0, false, false, bSilent );

			// Check whether the rubberband executable is present.
			if ( ! Filesystem::file_exists( Preferences::get_instance()->
											m_rubberBandCLIexecutable ) ) {
				rubberband.use = false;
			}
			pSample->set_rubberband( rubberband );
	
			// FIXME, kill EnvelopePoint, create Envelope class
			EnvelopePoint pt;

			Sample::VelocityEnvelope velocityEnvelope;
			XMLNode volumeNode = pNode->firstChildElement( "volume" );
			while ( ! volumeNode.isNull()  ) {
				pt.frame = volumeNode.read_int( "volume-position", 0, false, false, bSilent );
				pt.value = volumeNode.read_int( "volume-value", 0, false, false , bSilent);
				velocityEnvelope.push_back( pt );
				volumeNode = volumeNode.nextSiblingElement( "volume" );
			}
			pSample->set_velocity_envelope( velocityEnvelope );

			Sample::VelocityEnvelope panEnvelope;
			XMLNode panNode = pNode->firstChildElement( "pan" );
			while ( ! panNode.isNull()  ) {
				pt.frame = panNode.read_int( "pan-position", 0, false, false, bSilent );
				pt.value = panNode.read_int( "pan-value", 0, false, false, bSilent );
				panEnvelope.push_back( pt );
				panNode = panNode.nextSiblingElement( "pan" );
			}
			pSample->set_pan_envelope( panEnvelope );
		}
	}
	
	auto pLayer = std::make_shared<InstrumentLayer>( pSample );
	pLayer->set_start_velocity( pNode->read_float( "min", 0.0,
												   true, true, bSilent  ) );
	pLayer->set_end_velocity( pNode->read_float( "max", 1.0,
												 true, true, bSilent ) );
	pLayer->set_gain( pNode->read_float( "gain", 1.0,
										 true, false, bSilent ) );
	pLayer->set_pitch( pNode->read_float( "pitch", 0.0,
										  true, false, bSilent ) );
	return pLayer;
}

void InstrumentLayer::save_to( XMLNode* node, bool bFull )
{
	auto pSample = get_sample();
	if ( pSample == nullptr ) {
		ERRORLOG( "No sample associated with layer. Skipping it" );
		return;
	}
	
	XMLNode layer_node = node->createNode( "layer" );

	QString sFilename;
	if ( bFull ) {
		sFilename = Filesystem::prepare_sample_path( pSample->get_filepath() );
	} else {
		sFilename = get_sample()->get_filename();
	}
	
	layer_node.write_string( "filename", sFilename );
	layer_node.write_float( "min", __start_velocity );
	layer_node.write_float( "max", __end_velocity );
	layer_node.write_float( "gain", __gain );
	layer_node.write_float( "pitch", __pitch );

	if ( bFull ) {
		layer_node.write_bool( "ismodified", pSample->get_is_modified() );
		layer_node.write_string( "smode", pSample->get_loop_mode_string() );

		Sample::Loops loops = pSample->get_loops();
		layer_node.write_int( "startframe", loops.start_frame );
		layer_node.write_int( "loopframe", loops.loop_frame );
		layer_node.write_int( "loops", loops.count );
		layer_node.write_int( "endframe", loops.end_frame );

		Sample::Rubberband rubberband = pSample->get_rubberband();
		layer_node.write_int( "userubber", static_cast<int>(rubberband.use) );
		layer_node.write_float( "rubberdivider", rubberband.divider );
		layer_node.write_int( "rubberCsettings", rubberband.c_settings );
		layer_node.write_float( "rubberPitch", rubberband.pitch );

		for ( const auto& velocity : *pSample->get_velocity_envelope() ) {
			XMLNode volumeNode = layer_node.createNode( "volume" );
			volumeNode.write_int( "volume-position", velocity.frame );
			volumeNode.write_int( "volume-value", velocity.value );
		}

		for ( const auto& pan : *pSample->get_pan_envelope() ) {
			XMLNode panNode = layer_node.createNode( "pan" );
			panNode.write_int( "pan-position", pan.frame );
			panNode.write_int( "pan-value", pan.value );
		}
	}
}

QString InstrumentLayer::toQString( const QString& sPrefix, bool bShort ) const {
	QString s = Base::sPrintIndention;
	QString sOutput;
	if ( ! bShort ) {
		sOutput = QString( "%1[InstrumentLayer]\n" ).arg( sPrefix )
			.append( QString( "%1%2gain: %3\n" ).arg( sPrefix ).arg( s ).arg( __gain ) )
			.append( QString( "%1%2pitch: %3\n" ).arg( sPrefix ).arg( s ).arg( __pitch ) )
			.append( QString( "%1%2start_velocity: %3\n" ).arg( sPrefix ).arg( s ).arg( __start_velocity ) )
			.append( QString( "%1%2end_velocity: %3\n" ).arg( sPrefix ).arg( s ).arg( __end_velocity ) )
			.append( QString( "%1" ).arg( __sample->toQString( sPrefix + s, bShort ) ) );
	} else {
		sOutput = QString( "[InstrumentLayer]" )
			.append( QString( " gain: %1" ).arg( __gain ) )
			.append( QString( ", pitch: %1" ).arg( __pitch ) )
			.append( QString( ", start_velocity: %1" ).arg( __start_velocity ) )
			.append( QString( ", end_velocity: %1" ).arg( __end_velocity ) )
			.append( QString( ", sample: %1\n" ).arg( __sample->get_filepath() ) );
	}
	
	return sOutput;
}

};

/* vim: set softtabstop=4 noexpandtab: */
