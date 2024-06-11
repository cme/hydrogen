/*
 * Hydrogen
 * Copyright(c) 2002-2008 by Alex >Comix< Cominu [comix@users.sourceforge.net]
 * Copyright(c) 2008-2024 The hydrogen development team [hydrogen-devel@lists.sourceforge.net]
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

#include <core/CoreActionController.h>
#include <core/AudioEngine/AudioEngineTests.h>
#include <core/Hydrogen.h>
#include <core/Preferences/Preferences.h>
#include <core/Helpers/Filesystem.h>

#include <iostream>

#include "TransportTestsTimebase.h"
#include "TestHelper.h"

#include "assertions/AudioFile.h"

using namespace H2Core;

void TransportTestsTimebase::testFrameToTickConversion() {
	___INFOLOG( "" );
	auto pHydrogen = Hydrogen::get_instance();

	auto pSongDemo = Song::load( QString( "%1/GM_kit_demo3.h2song" )
								   .arg( Filesystem::demos_dir() ) );
	CPPUNIT_ASSERT( pSongDemo != nullptr );
	CoreActionController::setSong( pSongDemo );

	const std::vector<int> indices{ 0, 5, 7, 12 };
	for ( const int ii : indices ) {
		TestHelper::varyAudioDriverConfig( ii );
		perform( &AudioEngineTests::testFrameToTickConversion );
	}
	___INFOLOG( "passed" );
}

void TransportTestsTimebase::testTransportProcessing() {
	___INFOLOG( "" );
	auto pHydrogen = Hydrogen::get_instance();

	auto pSongDemo = Song::load( QString( "%1/GM_kit_demo3.h2song" )
								   .arg( Filesystem::demos_dir() ) );
	CPPUNIT_ASSERT( pSongDemo != nullptr );
	CoreActionController::setSong( pSongDemo );

	const std::vector<int> indices{ 1, 9, 14 };
	for ( const int ii : indices ) {
		TestHelper::varyAudioDriverConfig( ii );
		perform( &AudioEngineTests::testTransportProcessing );
	}
	___INFOLOG( "passed" );
}

void TransportTestsTimebase::testTransportProcessingTimeline() {
	___INFOLOG( "" );
	auto pHydrogen = Hydrogen::get_instance();

	auto pSongTransportProcessingTimeline =
		Song::load( QString( H2TEST_FILE( "song/AE_transportProcessingTimeline.h2song" ) ) );
	CPPUNIT_ASSERT( pSongTransportProcessingTimeline != nullptr );
	CoreActionController::setSong( pSongTransportProcessingTimeline );

	const std::vector<int> indices{ 2, 9, 10 };
	for ( const int ii : indices ) {
		TestHelper::varyAudioDriverConfig( ii );
		perform( &AudioEngineTests::testTransportProcessingTimeline );
	}
	___INFOLOG( "passed" );
}		
 
void TransportTestsTimebase::testTransportRelocation() {
	___INFOLOG( "" );
	auto pHydrogen = Hydrogen::get_instance();

	auto pSongDemo = Song::load( QString( "%1/GM_kit_demo3.h2song" )
								   .arg( Filesystem::demos_dir() ) );
	CPPUNIT_ASSERT( pSongDemo != nullptr );
	CoreActionController::setSong( pSongDemo );
	
	CoreActionController::activateTimeline( true );
	CoreActionController::addTempoMarker( 0, 120 );
	CoreActionController::addTempoMarker( 1, 100 );
	CoreActionController::addTempoMarker( 2, 20 );
	CoreActionController::addTempoMarker( 3, 13.4 );
	CoreActionController::addTempoMarker( 4, 383.2 );
	CoreActionController::addTempoMarker( 5, 64.38372 );
	CoreActionController::addTempoMarker( 6, 96.3 );
	CoreActionController::addTempoMarker( 7, 240.46 );
	CoreActionController::addTempoMarker( 8, 200.1 );
	
	const std::vector<int> indices{ 0, 5, 6 };
	for ( const int ii : indices ) {
		TestHelper::varyAudioDriverConfig( ii );
		perform( &AudioEngineTests::testTransportRelocation );
	}

	CoreActionController::activateTimeline( false );
	___INFOLOG( "passed" );
}

void TransportTestsTimebase::testLoopMode() {
	___INFOLOG( "" );

	const QString sSongFile = H2TEST_FILE( "song/AE_loopMode.h2song" );

	auto pHydrogen = H2Core::Hydrogen::get_instance();

	auto pSong = H2Core::Song::load( sSongFile );
	CPPUNIT_ASSERT( pSong != nullptr );

	CoreActionController::setSong( pSong );

	const std::vector<int> indices{ 0, 1, 12 };
	for ( const int ii : indices ) {
		TestHelper::varyAudioDriverConfig( ii );
		perform( &AudioEngineTests::testLoopMode );
	}
	___INFOLOG( "passed" );
}

void TransportTestsTimebase::testSongSizeChange() {
	___INFOLOG( "" );
	auto pHydrogen = Hydrogen::get_instance();

	auto pSongSizeChanged =
		Song::load( QString( H2TEST_FILE( "song/AE_songSizeChanged.h2song" ) ) );
	CPPUNIT_ASSERT( pSongSizeChanged != nullptr );
	CoreActionController::setSong( pSongSizeChanged );

	// Depending on buffer size and sample rate transport might be
	// loop when toggling a pattern at the end of the song. If there
	// were tempo markers present, the chunk of the interval covered
	// by AudioEngine::computeTickInterval being looped would have a
	// different tickSize than its first part. This is itself no
	// problem but it would make the test much more complex as we test
	// against those calculated intervals to remain constant.
	CoreActionController::activateTimeline( false );

	const std::vector<int> indices{ 0, 1, 2, 3 };
	for ( const int ii : indices ) {
		TestHelper::varyAudioDriverConfig( ii );
		
		// For larger sample rates no notes will remain in the
		// AudioEngine::m_songNoteQueue after one process step.
		if ( H2Core::Preferences::get_instance()->m_nSampleRate <= 48000 ) {
			perform( &AudioEngineTests::testSongSizeChange );
		}
	}
	
	CoreActionController::activateLoopMode( false );
	___INFOLOG( "passed" );
}		

void TransportTestsTimebase::testSongSizeChangeInLoopMode() {
	___INFOLOG( "" );
	auto pHydrogen = Hydrogen::get_instance();

	auto pSongDemo = Song::load( QString( "%1/GM_kit_demo3.h2song" )
								   .arg( Filesystem::demos_dir() ) );
	CPPUNIT_ASSERT( pSongDemo != nullptr );
	CoreActionController::setSong( pSongDemo );

	const std::vector<int> indices{ 0, 5, 7, 13 };
	for ( const int ii : indices ) {
		TestHelper::varyAudioDriverConfig( ii );
		perform( &AudioEngineTests::testSongSizeChangeInLoopMode );
	}
	___INFOLOG( "passed" );
}

void TransportTestsTimebase::testPlaybackTrack() {
	___INFOLOG( "" );

	QString sSongFile = H2TEST_FILE( "song/AE_playbackTrack.h2song" );
	QString sOutFile = Filesystem::tmp_file_path("testPlaybackTrack.wav");
	QString sRefFile = H2TEST_FILE("song/res/playbackTrack.flac");

	TestHelper::exportSong( sSongFile, sOutFile );
	H2TEST_ASSERT_AUDIO_FILES_EQUAL( sRefFile, sOutFile );
	Filesystem::rm( sOutFile );
	___INFOLOG( "passed" );
}

void TransportTestsTimebase::testSampleConsistency() {
	___INFOLOG( "" );

	const QString sSongFile = H2TEST_FILE( "song/AE_sampleConsistency.h2song" );
	const QString sDrumkitDir = H2TEST_FILE( "drumkits/sampleKit/" );
	const QString sOutFile = Filesystem::tmp_file_path("testsampleConsistency.wav");
	const QString sRefFile = H2TEST_FILE("drumkits/sampleKit/longSample.flac");

	auto pHydrogen = H2Core::Hydrogen::get_instance();

	auto pSong = H2Core::Song::load( sSongFile );

	CPPUNIT_ASSERT( pSong != nullptr );
		
	pHydrogen->setSong( pSong );

	// Apply drumkit containing the long sample to be tested.
	CoreActionController::setDrumkit( sDrumkitDir, true );
	
	TestHelper::exportSong( sOutFile );
	H2TEST_ASSERT_AUDIO_FILES_DATA_EQUAL( sRefFile, sOutFile );
	Filesystem::rm( sOutFile );
	___INFOLOG( "passed" );
}

void TransportTestsTimebase::testNoteEnqueuing() {
	___INFOLOG( "" );
	auto pHydrogen = Hydrogen::get_instance();

	auto pSongNoteEnqueuing =
		Song::load( QString( H2TEST_FILE( "song/AE_noteEnqueuing.h2song" ) ) );
	CPPUNIT_ASSERT( pSongNoteEnqueuing != nullptr );
	CoreActionController::setSong( pSongNoteEnqueuing );

	// This test is quite time consuming.
	std::vector<int> indices{ 1, 9, 12 };
	for ( auto ii : indices ) {
		TestHelper::varyAudioDriverConfig( ii );
		perform( &AudioEngineTests::testNoteEnqueuing );
	}
	___INFOLOG( "passed" );
}

void TransportTestsTimebase::testNoteEnqueuingTimeline() {
	___INFOLOG( "" );
	auto pHydrogen = Hydrogen::get_instance();
	auto pSong = Song::load( QString( H2TEST_FILE( "song/AE_noteEnqueuingTimeline.h2song" ) ) );

	CPPUNIT_ASSERT( pSong != nullptr );

	CoreActionController::setSong( pSong );

	// This test is quite time consuming.
	std::vector<int> indices{ 0, 5, 7 };

	for ( auto ii : indices ) {
		TestHelper::varyAudioDriverConfig( ii );
		perform( &AudioEngineTests::testNoteEnqueuingTimeline );
	}
	___INFOLOG( "passed" );
}

void TransportTestsTimebase::testHumanization() {
	___INFOLOG( "" );
	auto pHydrogen = Hydrogen::get_instance();

	auto pSongHumanization =
		Song::load( QString( H2TEST_FILE( "song/AE_humanization.h2song" ) ) );
	CPPUNIT_ASSERT( pSongHumanization != nullptr );
	CoreActionController::setSong( pSongHumanization );

	// This test is quite time consuming.
	std::vector<int> indices{ 1, 10 };
	for ( auto ii : indices ) {
		TestHelper::varyAudioDriverConfig( ii );
		perform( &AudioEngineTests::testHumanization );
	}
	___INFOLOG( "passed" );
}

void TransportTestsTimebase::perform( std::function<void()> func ) {
	try {
		func();
	} catch ( std::exception& err ) {
		CppUnit::Message msg( err.what() );
		throw CppUnit::Exception( msg );
	}
}
