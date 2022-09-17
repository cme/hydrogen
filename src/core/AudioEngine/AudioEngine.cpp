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

#include <core/AudioEngine/AudioEngine.h>

#ifdef WIN32
#    include "core/Timehelper.h"
#else
#    include <unistd.h>
#    include <sys/time.h>
#endif

#include <core/EventQueue.h>
#include <core/FX/Effects.h>
#include <core/Basics/Song.h>
#include <core/Basics/Pattern.h>
#include <core/Basics/PatternList.h>
#include <core/Basics/Note.h>
#include <core/Basics/DrumkitComponent.h>
#include <core/Basics/AutomationPath.h>
#include <core/Basics/InstrumentList.h>
#include <core/Basics/InstrumentLayer.h>
#include <core/Basics/InstrumentComponent.h>
#include <core/Sampler/Sampler.h>
#include <core/Helpers/Filesystem.h>

#include <core/IO/AudioOutput.h>
#include <core/IO/JackAudioDriver.h>
#include <core/IO/NullDriver.h>
#include <core/IO/MidiInput.h>
#include <core/IO/MidiOutput.h>
#include <core/IO/CoreMidiDriver.h>
#include <core/IO/OssDriver.h>
#include <core/IO/FakeDriver.h>
#include <core/IO/AlsaAudioDriver.h>
#include <core/IO/PortAudioDriver.h>
#include <core/IO/DiskWriterDriver.h>
#include <core/IO/AlsaMidiDriver.h>
#include <core/IO/JackMidiDriver.h>
#include <core/IO/PortMidiDriver.h>
#include <core/IO/CoreAudioDriver.h>
#include <core/IO/PulseAudioDriver.h>

#include <core/Hydrogen.h>	// TODO: remove this line as soon as possible
#include <core/Preferences/Preferences.h>
#include <cassert>
#include <limits>
#include <random>

namespace H2Core
{

const int AudioEngine::nMaxTimeHumanize = 2000;

inline int randomValue( int max )
{
	return rand() % max;
}

inline float getGaussian( float z )
{
	// gaussian distribution -- dimss
	float x1, x2, w;
	do {
		x1 = 2.0 * ( ( ( float ) rand() ) / static_cast<float>(RAND_MAX) ) - 1.0;
		x2 = 2.0 * ( ( ( float ) rand() ) / static_cast<float>(RAND_MAX) ) - 1.0;
		w = x1 * x1 + x2 * x2;
	} while ( w >= 1.0 );

	w = sqrtf( ( -2.0 * logf( w ) ) / w );
	return x1 * w * z + 0.0; // tunable
}


/** Gets the current time.
 * \return Current time obtained by gettimeofday()*/
inline timeval currentTime2()
{
	struct timeval now;
	gettimeofday( &now, nullptr );
	return now;
}

AudioEngine::AudioEngine()
		: TransportInfo()
		, m_pSampler( nullptr )
		, m_pSynth( nullptr )
		, m_pAudioDriver( nullptr )
		, m_pMidiDriver( nullptr )
		, m_pMidiDriverOut( nullptr )
		, m_state( State::Initialized )
		, m_pMetronomeInstrument( nullptr )
		, m_nPatternStartTick( 0 )
		, m_nPatternTickPosition( 0 )
		, m_nPatternSize( MAX_NOTES )
		, m_fSongSizeInTicks( 0 )
		, m_nRealtimeFrames( 0 )
		, m_fMasterPeak_L( 0.0f )
		, m_fMasterPeak_R( 0.0f )
		, m_nColumn( -1 )
		, m_nextState( State::Ready )
		, m_fProcessTime( 0.0f )
		, m_fLadspaTime( 0.0f )
		, m_fMaxProcessTime( 0.0f )
		, m_fNextBpm( 120 )
		, m_pLocker({nullptr, 0, nullptr})
		, m_currentTickTime( {0,0})
		, m_fTickMismatch( 0 )
		, m_fLastTickIntervalEnd( -1 )
		, m_nFrameOffset( 0 )
		, m_fTickOffset( 0 )
{

	
	m_pSampler = new Sampler;
	m_pSynth = new Synth;
	
	gettimeofday( &m_currentTickTime, nullptr );
	
	m_pEventQueue = EventQueue::get_instance();
	
	srand( time( nullptr ) );

	// Create metronome instrument
	// Get the path to the file of the metronome sound.
	QString sMetronomeFilename = Filesystem::click_file_path();
	m_pMetronomeInstrument = std::make_shared<Instrument>( METRONOME_INSTR_ID, "metronome" );
	
	auto pLayer = std::make_shared<InstrumentLayer>( Sample::load( sMetronomeFilename ) );
	auto pCompo = std::make_shared<InstrumentComponent>( 0 );
	pCompo->set_layer(pLayer, 0);
	m_pMetronomeInstrument->get_components()->push_back( pCompo );
	m_pMetronomeInstrument->set_is_metronome_instrument(true);
	
	m_pPlayingPatterns = new PatternList();
	m_pPlayingPatterns->setNeedsLock( true );
	m_pNextPatterns = new PatternList();
	m_pNextPatterns->setNeedsLock( true );
	
	m_AudioProcessCallback = &audioEngine_process;

#ifdef H2CORE_HAVE_LADSPA
	Effects::create_instance();
#endif
}

AudioEngine::~AudioEngine()
{
	stopAudioDrivers();
	if ( getState() != State::Initialized ) {
		ERRORLOG( "Error the audio engine is not in State::Initialized" );
		return;
	}
	m_pSampler->stopPlayingNotes();

	this->lock( RIGHT_HERE );
	INFOLOG( "*** Hydrogen audio engine shutdown ***" );

	clearNoteQueue();

	// change the current audio engine state
	setState( State::Uninitialized );

	delete m_pPlayingPatterns;
	m_pPlayingPatterns = nullptr;

	delete m_pNextPatterns;
	m_pNextPatterns = nullptr;

	m_pMetronomeInstrument = nullptr;

	this->unlock();
	
#ifdef H2CORE_HAVE_LADSPA
	delete Effects::get_instance();
#endif

//	delete Sequencer::get_instance();
	delete m_pSampler;
	delete m_pSynth;
}

Sampler* AudioEngine::getSampler() const
{
	assert(m_pSampler);
	return m_pSampler;
}

Synth* AudioEngine::getSynth() const
{
	assert(m_pSynth);
	return m_pSynth;
}

void AudioEngine::lock( const char* file, unsigned int line, const char* function )
{
	#ifdef H2CORE_HAVE_DEBUG
	if ( __logger->should_log( Logger::Locks ) ) {
		__logger->log( Logger::Locks, _class_name(), __FUNCTION__,
					   QString( "by %1 : %2 : %3" ).arg( function ).arg( line ).arg( file ) );
	}
	#endif

	m_EngineMutex.lock();
	m_pLocker.file = file;
	m_pLocker.line = line;
	m_pLocker.function = function;
	m_LockingThread = std::this_thread::get_id();
}

bool AudioEngine::tryLock( const char* file, unsigned int line, const char* function )
{
	#ifdef H2CORE_HAVE_DEBUG
	if ( __logger->should_log( Logger::Locks ) ) {
		__logger->log( Logger::Locks, _class_name(), __FUNCTION__,
					   QString( "by %1 : %2 : %3" ).arg( function ).arg( line ).arg( file ) );
	}
	#endif
	bool res = m_EngineMutex.try_lock();
	if ( !res ) {
		// Lock not obtained
		return false;
	}
	m_pLocker.file = file;
	m_pLocker.line = line;
	m_pLocker.function = function;
	m_LockingThread = std::this_thread::get_id();
	#ifdef H2CORE_HAVE_DEBUG
	if ( __logger->should_log( Logger::Locks ) ) {
		__logger->log( Logger::Locks, _class_name(), __FUNCTION__, QString( "locked" ) );
	}
	#endif
	return true;
}

bool AudioEngine::tryLockFor( std::chrono::microseconds duration, const char* file, unsigned int line, const char* function )
{
	#ifdef H2CORE_HAVE_DEBUG
	if ( __logger->should_log( Logger::Locks ) ) {
		__logger->log( Logger::Locks, _class_name(), __FUNCTION__,
					   QString( "by %1 : %2 : %3" ).arg( function ).arg( line ).arg( file ) );
	}
	#endif
	bool res = m_EngineMutex.try_lock_for( duration );
	if ( !res ) {
		// Lock not obtained
		WARNINGLOG( QString( "Lock timeout: lock timeout %1:%2:%3, lock held by %4:%5:%6" )
					.arg( file ).arg( function ).arg( line )
					.arg( m_pLocker.file ).arg( m_pLocker.function ).arg( m_pLocker.line ));
		return false;
	}
	m_pLocker.file = file;
	m_pLocker.line = line;
	m_pLocker.function = function;
	m_LockingThread = std::this_thread::get_id();
	
	#ifdef H2CORE_HAVE_DEBUG
	if ( __logger->should_log( Logger::Locks ) ) {
		__logger->log( Logger::Locks, _class_name(), __FUNCTION__, QString( "locked" ) );
	}
	#endif
	return true;
}

void AudioEngine::unlock()
{
	// Leave "__locker" dirty.
	m_LockingThread = std::thread::id();
	m_EngineMutex.unlock();
	#ifdef H2CORE_HAVE_DEBUG
	if ( __logger->should_log( Logger::Locks ) ) {
		__logger->log( Logger::Locks, _class_name(), __FUNCTION__, QString( "" ) );
	}
	#endif
}

void AudioEngine::startPlayback()
{
	INFOLOG( "" );

	// check current state
	if ( getState() != State::Ready ) {
	   ERRORLOG( "Error the audio engine is not in State::Ready" );
		return;
	}

	// change the current audio engine state
	setState( State::Playing );

	// The locking of the pattern editor only takes effect if the
	// transport is rolling.
	handleSelectedPattern();
}

void AudioEngine::stopPlayback()
{
	INFOLOG( "" );

	// check current state
	if ( getState() != State::Playing ) {
		ERRORLOG( QString( "Error the audio engine is not in State::Playing but [%1]" )
				  .arg( static_cast<int>( getState() ) ) );
		return;
	}

	setState( State::Ready );
}

void AudioEngine::reset( bool bWithJackBroadcast ) {
	const auto pHydrogen = Hydrogen::get_instance();
	
	m_fMasterPeak_L = 0.0f;
	m_fMasterPeak_R = 0.0f;

	setFrames( 0 );
	setTick( 0 );
	setColumn( -1 );
	m_nPatternStartTick = 0;
	m_nPatternTickPosition = 0;
	m_fTickMismatch = 0;
	m_nFrameOffset = 0;
	m_fTickOffset = 0;
	m_fLastTickIntervalEnd = -1;

	updateBpmAndTickSize();
	
	clearNoteQueue();
	
#ifdef H2CORE_HAVE_JACK
	if ( pHydrogen->hasJackTransport() && bWithJackBroadcast ) {
		// Tell all other JACK clients to relocate as well. This has
		// to be called after updateFrames().
		static_cast<JackAudioDriver*>( m_pAudioDriver )->locateTransport( 0 );
	}
#endif
}

float AudioEngine::computeTickSize( const int nSampleRate, const float fBpm, const int nResolution)
{
	float fTickSize = nSampleRate * 60.0 / fBpm / nResolution;
	
	return fTickSize;
}

double AudioEngine::computeDoubleTickSize( const int nSampleRate, const float fBpm, const int nResolution)
{
	double fTickSize = static_cast<double>(nSampleRate) * 60.0 /
		static_cast<double>(fBpm) /
		static_cast<double>(nResolution);
	
	return fTickSize;
}

long long AudioEngine::computeFrame( double fTick, float fTickSize ) {
	return std::round( fTick * fTickSize );
}

double AudioEngine::computeTick( long long nFrame, float fTickSize ) {
	return nFrame / fTickSize;
}

float AudioEngine::getElapsedTime() const {
	
	const auto pHydrogen = Hydrogen::get_instance();
	const auto pDriver = pHydrogen->getAudioOutput();
	
	if ( pDriver == nullptr || pDriver->getSampleRate() == 0 ) {
		return 0;
	}

	return ( getFrames() - m_nFrameOffset )/ static_cast<float>(pDriver->getSampleRate());
}

void AudioEngine::locate( const double fTick, bool bWithJackBroadcast ) {
	const auto pHydrogen = Hydrogen::get_instance();
	const auto pDriver = pHydrogen->getAudioOutput();

	long long nNewFrame;

	// DEBUGLOG( QString( "fTick: %1" ).arg( fTick ) );

#ifdef H2CORE_HAVE_JACK
	// In case Hydrogen is using the JACK server to sync transport, it
	// has to be up to the server to relocate to a different
	// position.
	if ( pHydrogen->hasJackTransport() && bWithJackBroadcast ) {
		nNewFrame = computeFrameFromTick( fTick, &m_fTickMismatch );
		static_cast<JackAudioDriver*>( m_pAudioDriver )->locateTransport( nNewFrame );
		return;
	}
#endif

	reset( false );
	nNewFrame = computeFrameFromTick( fTick, &m_fTickMismatch );
	
	setFrames( nNewFrame );
	updateTransportPosition( fTick );
}

void AudioEngine::locateToFrame( const long long nFrame ) {
	const auto pHydrogen = Hydrogen::get_instance();
	
	reset( false );

	double fNewTick = computeTickFromFrame( nFrame );

	// As the tick mismatch is lost when converting a sought location
	// from ticks into frames, sending it to the JACK server,
	// receiving it in a callback, and providing it here, we will use
	// a heuristic in order to not experience any glitches upon
	// relocation.
	if ( std::fmod( fNewTick, std::floor( fNewTick ) ) >= 0.97 ) {
		INFOLOG( QString( "Computed tick [%1] will be rounded to [%2] in order to avoid glitches" )
				 .arg( fNewTick ).arg( std::round( fNewTick ) ) );
		fNewTick = std::round( fNewTick );
	}

	// Important step to assure the tick mismatch is set and
	// tick<->frame can be converted properly.
	long long nNewFrame = computeFrameFromTick( fNewTick, &m_fTickMismatch );
	if ( nNewFrame != nFrame ) {
		ERRORLOG( QString( "Something went wrong: nFrame: %1, nNewFrame: %2, fNewTick: %3, m_fTickMismatch: %4" )
				  .arg( nFrame )
				  .arg( nNewFrame )
				  .arg( fNewTick )
				  .arg( m_fTickMismatch ) );
	}
	setFrames( nNewFrame );
	
	updateTransportPosition( fNewTick );

	// While the locate function is wrapped by a caller in the
	// CoreActionController - which takes care of queuing the
	// relocation event - this function is only meant to be used in
	// very specific circumstances and is not that nicely wrapped.
	EventQueue::get_instance()->push_event( EVENT_RELOCATION, 0 );
}

void AudioEngine::incrementTransportPosition( uint32_t nFrames ) {
	auto pSong = Hydrogen::get_instance()->getSong();

	if ( pSong == nullptr ) {
		return;
	}	

	setFrames( getFrames() + nFrames );

	double fNewTick = computeTickFromFrame( getFrames() );
	m_fTickMismatch = 0;
		
	// DEBUGLOG( QString( "nFrames: %1, old frames: %2, getDoubleTick(): %3, newTick: %4, ticksize: %5" )
	// 		  .arg( nFrames )
	// 		  .arg( getFrames() - nFrames )
	// 		  .arg( getDoubleTick(), 0, 'f' )
	// 		  .arg( fNewTick, 0, 'f' )
	// 		  .arg( getTickSize(), 0, 'f' ) );

	updateTransportPosition( fNewTick );
}

void AudioEngine::updateTransportPosition( double fTick ) {

	const auto pHydrogen = Hydrogen::get_instance();
	const auto pSong = pHydrogen->getSong();
	const auto pDriver = pHydrogen->getAudioOutput();

	assert( pSong );

	// WARNINGLOG( QString( "[Before] frame: %5, tick: %1, pTickPos: %2, pStartPos: %3, column: %4, provided ticks: %6" )
	// 			.arg( getDoubleTick(), 0, 'f' )
	// 			.arg( m_nPatternTickPosition )
	// 			.arg( m_nPatternStartTick )
	// 			.arg( m_nColumn )
	// 			.arg( getFrames() )
	// 			.arg( fTick, 0, 'f' ) );

	// Update m_nPatternStartTick, m_nPatternTickPosition, and
	// m_nPatternSize.
	if ( pHydrogen->getMode() == Song::Mode::Song ) {
		updateSongTransportPosition( fTick );
	}
	else if ( pHydrogen->getMode() == Song::Mode::Pattern ) {

		// If the transport is rolling, pattern tick variables were
		// already updated in the call to updateNoteQueue.
		if ( getState() != State::Playing ) {
			updatePatternTransportPosition( fTick );
		}
	}
	
	setTick( fTick );
	
	updateBpmAndTickSize();
	
	// WARNINGLOG( QString( "[After] frame: %5, tick: %1, pTickPos: %2, pStartPos: %3, column: %4, provided ticks: %6" )
	// 			.arg( getDoubleTick(), 0, 'f' )
	// 			.arg( m_nPatternTickPosition )
	// 			.arg( m_nPatternStartTick )
	// 			.arg( m_nColumn )
	// 			.arg( getFrames() )
	// 			.arg( fTick, 0, 'f' ) );
	
}

void AudioEngine::updatePatternTransportPosition( double fTick ) {

	auto pHydrogen = Hydrogen::get_instance();

	// In selected pattern mode we update the pattern size _before_
	// checking the whether transport reached the end of a
	// pattern. This way when switching from a shorter to one double
	// its size transport will just continue till the end of the new,
	// longer one. If we would not update pattern size, transport
	// would be looped at half the length of the newly selected
	// pattern, which looks like a glitch.
	//
	// The update of the playing pattern is done asynchronous in
	// Hydrogen::setSelectedPatternNumber() and does not have to
	// queried in here in each run.
	// if ( pHydrogen->getPatternMode() == Song::PatternMode::Selected ) {
	// 	updatePlayingPatterns( 0, fTick );
	// }

	// Transport went past the end of the pattern or Pattern mode
	// was just activated.
	if ( fTick >= static_cast<double>(m_nPatternStartTick + m_nPatternSize) ||
		 fTick < static_cast<double>(m_nPatternStartTick) ) {
		m_nPatternStartTick +=
			static_cast<long>(std::floor( ( fTick -
											static_cast<double>(m_nPatternStartTick) ) /
										  static_cast<double>(m_nPatternSize) )) *
			m_nPatternSize;

		// In stacked pattern mode we will only update the playing
		// patterns if the transport of the original pattern is
		// looped. This way all patterns start fresh at the beginning.
		if ( pHydrogen->getPatternMode() == Song::PatternMode::Stacked ) {
			// Updates m_nPatternSize.
			updatePlayingPatterns( 0, fTick );
		}
	}

	m_nPatternTickPosition = static_cast<long>(std::floor( fTick )) -
		m_nPatternStartTick;
	if ( m_nPatternTickPosition > m_nPatternSize ) {
		m_nPatternTickPosition = ( static_cast<long>(std::floor( fTick ))
								   - m_nPatternStartTick ) %
			m_nPatternSize;
	}
}

void AudioEngine::updateSongTransportPosition( double fTick ) {

	auto pHydrogen = Hydrogen::get_instance();
	const auto pSong = pHydrogen->getSong();

	if ( fTick < 0 ) {
		ERRORLOG( QString( "Provided tick [%1] is negative!" )
				  .arg( fTick, 0, 'f' ) );
		return;
	}
	
	int nNewColumn = pHydrogen->getColumnForTick( std::floor( fTick ),
												  pSong->isLoopEnabled(),
												  &m_nPatternStartTick );

	// While the current tick position is constantly increasing,
	// m_nPatternStartTick is only defined between 0 and
	// m_fSongSizeInTicks. We will take care of the looping next.
	if ( fTick >= m_fSongSizeInTicks &&
		 m_fSongSizeInTicks != 0 ) {
		
		m_nPatternTickPosition =
			std::fmod( std::floor( fTick ) - m_nPatternStartTick,
					   m_fSongSizeInTicks );
	}
	else {
		m_nPatternTickPosition = std::floor( fTick ) - m_nPatternStartTick;
	}
	
	if ( m_nColumn != nNewColumn ) {
		setColumn( nNewColumn );
		updatePlayingPatterns( nNewColumn, 0 );
		handleSelectedPattern();
	}
}

void AudioEngine::updateBpmAndTickSize() {
	if ( ! ( m_state == State::Playing ||
			 m_state == State::Ready ||
			 m_state == State::Testing ) ) {
		return;
	}
	auto pHydrogen = Hydrogen::get_instance();
	auto pSong = pHydrogen->getSong();
	float fOldBpm = getBpm();
	
	float fNewBpm = getBpmAtColumn( pHydrogen->getAudioEngine()->getColumn() );
	if ( fNewBpm != getBpm() ) {
		setBpm( fNewBpm );
		EventQueue::get_instance()->push_event( EVENT_TEMPO_CHANGED, 0 );
	}

	float fOldTickSize = getTickSize();
	float fNewTickSize = AudioEngine::computeTickSize( static_cast<float>(m_pAudioDriver->getSampleRate()),
													   getBpm(), pSong->getResolution() );

	// DEBUGLOG(QString( "sample rate: %1, tick size: %2 -> %3, bpm: %4 -> %5" )
	// 		 .arg( static_cast<float>(m_pAudioDriver->getSampleRate()))
	// 		 .arg( fOldTickSize, 0, 'f' )
	// 		 .arg( fNewTickSize, 0, 'f' )
	// 		 .arg( fOldBpm, 0, 'f' )
	// 		 .arg( getBpm(), 0, 'f' ) );
	
	// Nothing changed - avoid recomputing
	if ( fNewTickSize == fOldTickSize ) {
		return;
	}

	if ( fNewTickSize == 0 ) {
		ERRORLOG( QString( "Something went wrong while calculating the tick size. [oldTS: %1, newTS: %2]" )
				  .arg( fOldTickSize, 0, 'f' ).arg( fNewTickSize, 0, 'f' ) );
		return;
	}
	
	setTickSize( fNewTickSize ); 

	if ( ! pHydrogen->isTimelineEnabled() ) {
		// If we deal with a single speed for the whole song, the frames
		// since the beginning of the song are tempo-dependent and have to
		// be recalculated.
		long long nNewFrames = computeFrameFromTick( getDoubleTick(), &m_fTickMismatch );
		m_nFrameOffset = nNewFrames - getFrames() + m_nFrameOffset;

		// DEBUGLOG( QString( "old frame: %1, new frame: %2, tick: %3, old tick size: %4, new tick size: %5" )
		// 		  .arg( getFrames() ).arg( nNewFrames ).arg( getDoubleTick(), 0, 'f' )
		// 		  .arg( fOldTickSize, 0, 'f' ).arg( fNewTickSize, 0, 'f' ) );
		
		setFrames( nNewFrames );

		// In addition, all currently processed notes have to be
		// updated to be still valid.
		handleTempoChange();
		
	} else if ( m_nFrameOffset != 0 ) {
		// In case the frame offset was already set, we have to update
		// it too in order to prevent inconsistencies in the transport
		// and audio rendering when switching off the Timeline during
		// playback, relocating, switching it on again, alter song
		// size or sample rate, and switching it off again. I know,
		// quite an edge case but still.
		// If we deal with a single speed for the whole song, the frames
		// since the beginning of the song are tempo-dependent and have to
		// be recalculated.
		long long nNewFrames = computeFrameFromTick( getDoubleTick(),
													 &m_fTickMismatch );
		m_nFrameOffset = nNewFrames - getFrames() + m_nFrameOffset;
	}
}
				
// This function uses the assumption that sample rate and resolution
// are constant over the whole song.
long long AudioEngine::computeFrameFromTick( const double fTick, double* fTickMismatch, int nSampleRate ) const {

	const auto pHydrogen = Hydrogen::get_instance();
	const auto pSong = pHydrogen->getSong();
	const auto pTimeline = pHydrogen->getTimeline();
	assert( pSong );

	if ( nSampleRate == 0 ) {
		nSampleRate = pHydrogen->getAudioOutput()->getSampleRate();
	}
	const int nResolution = pSong->getResolution();

	const double fTickSize = AudioEngine::computeDoubleTickSize( nSampleRate,
																getBpm(),
																nResolution );
	
	if ( nSampleRate == 0 || nResolution == 0 ) {
		ERRORLOG( "Not properly initialized yet" );
		*fTickMismatch = 0;
		return 0;
	}

	if ( fTick == 0 ) {
		*fTickMismatch = 0;
		return 0;
	}
		
	const auto tempoMarkers = pTimeline->getAllTempoMarkers();
	
	long long nNewFrames = 0;
	if ( pHydrogen->isTimelineEnabled() &&
		 ! ( tempoMarkers.size() == 1 &&
			 pTimeline->isFirstTempoMarkerSpecial() ) ) {

		double fNewTick = fTick;
		double fRemainingTicks = fTick;
		double fNextTick, fPassedTicks = 0;
		double fNextTickSize;
		double fNewFrames = 0;

		const int nColumns = pSong->getPatternGroupVector()->size();

		while ( fRemainingTicks > 0 ) {
		
			for ( int ii = 1; ii <= tempoMarkers.size(); ++ii ) {
				if ( ii == tempoMarkers.size() ||
					 tempoMarkers[ ii ]->nColumn >= nColumns ) {
					fNextTick = m_fSongSizeInTicks;
				} else {
					fNextTick =
						static_cast<double>(pHydrogen->getTickForColumn( tempoMarkers[ ii ]->nColumn ) );
				}

				fNextTickSize =
					AudioEngine::computeDoubleTickSize( nSampleRate,
														tempoMarkers[ ii - 1 ]->fBpm,
														nResolution );
				
				if ( fRemainingTicks > ( fNextTick - fPassedTicks ) ) {
					// The whole segment of the timeline covered by tempo
					// marker ii is left of the current transport position.
					fNewFrames += ( fNextTick - fPassedTicks ) * fNextTickSize;

					
					// DEBUGLOG( QString( "[segment] fTick: %1, fNewFrames: %2, fNextTick: %3, fRemainingTicks: %4, fPassedTicks: %5, fNextTickSize: %6, tempoMarkers[ ii - 1 ]->nColumn: %7, tempoMarkers[ ii - 1 ]->fBpm: %8, tick increment (fNextTick - fPassedTicks): %9, frame increment (fRemainingTicks * fNextTickSize): %10" )
					// 		  .arg( fTick, 0, 'f' )
					// 		  .arg( fNewFrames, 0, 'g', 30 )
					// 		  .arg( fNextTick, 0, 'f' )
					// 		  .arg( fRemainingTicks, 0, 'f' )
					// 		  .arg( fPassedTicks, 0, 'f' )
					// 		  .arg( fNextTickSize, 0, 'f' )
					// 		  .arg( tempoMarkers[ ii - 1 ]->nColumn )
					// 		  .arg( tempoMarkers[ ii - 1 ]->fBpm )
					// 		  .arg( fNextTick - fPassedTicks, 0, 'f' )
					// 		  .arg( ( fNextTick - fPassedTicks ) * fNextTickSize, 0, 'g', 30 )
					// 		  );
					
					fRemainingTicks -= fNextTick - fPassedTicks;
					
					fPassedTicks = fNextTick;

				}
				else {
					// The next frame is within this segment.
					fNewFrames += fRemainingTicks * fNextTickSize;

					nNewFrames = static_cast<long long>( std::round( fNewFrames ) );

					// Keep track of the rounding error to be able to
					// switch between fTick and its frame counterpart
					// later on.
					// In case fTick is located close to a tempo
					// marker we will only cover the part up to the
					// tempo marker in here as only this region is
					// governed by fNextTickSize.
					const double fRoundingErrorInTicks =
						( fNewFrames - static_cast<double>( nNewFrames ) ) /
						fNextTickSize;

					// Compares the negative distance between current
					// position (fNewFrames) and the one resulting
					// from rounding - fRoundingErrorInTicks - with
					// the negative distance between current position
					// (fNewFrames) and location of next tempo marker.
					if ( fRoundingErrorInTicks >
						 fPassedTicks + fRemainingTicks - fNextTick ) {
						// Whole mismatch located within the current
						// tempo interval.
						*fTickMismatch = fRoundingErrorInTicks;
					}
					else {
						// Mismatch at this side of the tempo marker.
						*fTickMismatch =
							fPassedTicks + fRemainingTicks - fNextTick;

						const double fFinalFrames = fNewFrames +
							( fNextTick - fPassedTicks - fRemainingTicks ) *
							fNextTickSize;

						// Mismatch located beyond the tempo marker.
						double fFinalTickSize;
						if ( ii < tempoMarkers.size() ) {
							fFinalTickSize =
								AudioEngine::computeDoubleTickSize( nSampleRate,
																	tempoMarkers[ ii ]->fBpm,
																	nResolution );
						}
						else {
							fFinalTickSize =
								AudioEngine::computeDoubleTickSize( nSampleRate,
																	tempoMarkers[ 0 ]->fBpm,
																	nResolution );
						}

						// DEBUGLOG( QString( "[mismatch] fTickMismatch: [%1 + %2], static_cast<double>(nNewFrames): %3, fNewFrames: %4, fFinalFrames: %5, fNextTickSize: %6, fPassedTicks: %7, fRemainingTicks: %8, fFinalTickSize: %9" )
						// 			.arg( fPassedTicks + fRemainingTicks - fNextTick )
						// 			.arg( ( fFinalFrames - static_cast<double>(nNewFrames) ) / fNextTickSize )
						// 			.arg( nNewFrames )
						// 			.arg( fNewFrames, 0, 'f' )
						// 			.arg( fFinalFrames, 0, 'f' )
						// 			.arg( fNextTickSize, 0, 'f' )
						// 			.arg( fPassedTicks, 0, 'f' )
						// 			.arg( fRemainingTicks, 0, 'f' )
						// 			.arg( fFinalTickSize, 0, 'f' ));
						
						*fTickMismatch += 
							( fFinalFrames - static_cast<double>(nNewFrames) ) /
							fFinalTickSize;
					}

					// DEBUGLOG( QString( "[end] fTick: %1, fNewFrames: %2, fNextTick: %3, fRemainingTicks: %4, fPassedTicks: %5, fNextTickSize: %6, tempoMarkers[ ii - 1 ]->nColumn: %7, tempoMarkers[ ii - 1 ]->fBpm: %8, nNewFrames: %9, fTickMismatch: %10, frame increment (fRemainingTicks * fNextTickSize): %11, fRoundingErrorInTicks: %12" )
					// 		  .arg( fTick, 0, 'f' )
					// 		  .arg( fNewFrames, 0, 'g', 30 )
					// 		  .arg( fNextTick, 0, 'f' )
					// 		  .arg( fRemainingTicks, 0, 'f' )
					// 		  .arg( fPassedTicks, 0, 'f' )
					// 		  .arg( fNextTickSize, 0, 'f' )
					// 		  .arg( tempoMarkers[ ii - 1 ]->nColumn )
					// 		  .arg( tempoMarkers[ ii - 1 ]->fBpm )
					// 		  .arg( nNewFrames )
					// 		  .arg( *fTickMismatch, 0, 'g', 30 )
					// 		  .arg( fRemainingTicks * fNextTickSize, 0, 'g', 30 )
					// 		  .arg( fRoundingErrorInTicks, 0, 'f' )
					// 		  );

					fRemainingTicks -= fNewTick - fPassedTicks;
					break;
				}
			}

			if ( fRemainingTicks != 0 ) {
				// The provided fTick is larger than the song. But,
				// luckily, we just calculated the song length in
				// frames (fNewFrames).
				const int nRepetitions = std::floor(fTick / m_fSongSizeInTicks);
				const double fSongSizeInFrames = fNewFrames;
				
				fNewFrames *= static_cast<double>(nRepetitions);
				fNewTick = std::fmod( fTick, m_fSongSizeInTicks );
				fRemainingTicks = fNewTick;
				fPassedTicks = 0;

				// DEBUGLOG( QString( "[repeat] frames covered: %1, ticks covered: %2, ticks remaining: %3, nRepetitions: %4, fSongSizeInFrames: %5" )
				// 		  .arg( fNewFrames, 0, 'g', 30 )
				// 		  .arg( fTick - fNewTick, 0, 'g', 30 )
				// 		  .arg( fRemainingTicks, 0, 'g', 30 )
				// 		  .arg( nRepetitions )
				// 		  .arg( fSongSizeInFrames, 0, 'g', 30 )
				// 		  );

				if ( std::isinf( fNewFrames ) ||
					 static_cast<long long>(fNewFrames) >
					 std::numeric_limits<long long>::max() ) {
					ERRORLOG( QString( "Provided ticks [%1] are too large." ).arg( fTick ) );
					return 0;
				}
			}
		}
	} else {
		
		// No Timeline but a single tempo for the whole song.
		const double fNewFrames = static_cast<double>(fTick) *
			fTickSize;
		nNewFrames = static_cast<long long>( std::round( fNewFrames ) );
		*fTickMismatch = ( fNewFrames - static_cast<double>(nNewFrames ) ) /
			fTickSize;

		// DEBUGLOG(QString("[no-timeline] nNewFrames: %1, fTick: %2, fTickSize: %3, fTickMismatch: %4" )
		// 		 .arg( nNewFrames ).arg( fTick, 0, 'f' ).arg( fTickSize, 0, 'f' )
		// 		 .arg( *fTickMismatch, 0, 'g', 30 ));
		
	}
	
	return nNewFrames;
}

double AudioEngine::computeTickFromFrame( const long long nFrame, int nSampleRate ) const {
	const auto pHydrogen = Hydrogen::get_instance();

	if ( nFrame < 0 ) {
		ERRORLOG( QString( "Provided frame [%1] must be non-negative" ).arg( nFrame ) );
	}
	
	const auto pSong = pHydrogen->getSong();
	const auto pTimeline = pHydrogen->getTimeline();
	assert( pSong );

	if ( nSampleRate == 0 ) {
		nSampleRate = pHydrogen->getAudioOutput()->getSampleRate();
	}
	const int nResolution = pSong->getResolution();
	double fTick = 0;

	const double fTickSize =
		AudioEngine::computeDoubleTickSize( nSampleRate,
											getBpm(),
											nResolution );
	
	if ( nSampleRate == 0 || nResolution == 0 ) {
		ERRORLOG( "Not properly initialized yet" );
		return fTick;
	}

	if ( nFrame == 0 ) {
		return fTick;
	}
		
	const auto tempoMarkers = pTimeline->getAllTempoMarkers();
	
	if ( pHydrogen->isTimelineEnabled() &&
		 ! ( tempoMarkers.size() == 1 &&
			 pTimeline->isFirstTempoMarkerSpecial() ) ) {

		// We are using double precision in here to avoid rounding
		// errors.
		const double fTargetFrames = static_cast<double>(nFrame);
		double fPassedFrames = 0;
		double fNextFrames = 0;
		double fNextTicks, fPassedTicks = 0;
		double fNextTickSize;
		long long nRemainingFrames;

		const int nColumns = pSong->getPatternGroupVector()->size();

		while ( fPassedFrames < fTargetFrames ) {
		
			for ( int ii = 1; ii <= tempoMarkers.size(); ++ii ) {

				fNextTickSize =
					AudioEngine::computeDoubleTickSize( nSampleRate,
														tempoMarkers[ ii - 1 ]->fBpm,
														nResolution );

				if ( ii == tempoMarkers.size() ||
					 tempoMarkers[ ii ]->nColumn >= nColumns ) {
					fNextTicks = m_fSongSizeInTicks;
				} else {
					fNextTicks =
						static_cast<double>(pHydrogen->getTickForColumn( tempoMarkers[ ii ]->nColumn ));
				}
				fNextFrames = (fNextTicks - fPassedTicks) * fNextTickSize;
		
				if ( fNextFrames < ( fTargetFrames -
									 fPassedFrames ) ) {
				   
					// DEBUGLOG(QString( "[segment] nFrame: %1, fTick: %2, nSampleRate: %3, fNextTickSize: %4, fNextTicks: %5, fNextFrames: %6, tempoMarkers[ ii -1 ]->nColumn: %7, tempoMarkers[ ii -1 ]->fBpm: %8, fPassedTicks: %9, fPassedFrames: %10, fNewTick (tick increment): %11, fNewTick * fNextTickSize (frame increment): %12" )
					// 		 .arg( nFrame )
					// 		 .arg( fTick, 0, 'f' )
					// 		 .arg( nSampleRate )
					// 		 .arg( fNextTickSize, 0, 'f' )
					// 		 .arg( fNextTicks, 0, 'f' )
					// 		 .arg( fNextFrames, 0, 'f' )
					// 		 .arg( tempoMarkers[ ii -1 ]->nColumn )
					// 		 .arg( tempoMarkers[ ii -1 ]->fBpm )
					// 		 .arg( fPassedTicks, 0, 'f' )
					// 		 .arg( fPassedFrames, 0, 'f' )
					// 		 .arg( fNextTicks - fPassedTicks, 0, 'f' )
					// 		 .arg( (fNextTicks - fPassedTicks) * fNextTickSize, 0, 'g', 30 )
					// 		 );
					
					// The whole segment of the timeline covered by tempo
					// marker ii is left of the transport position.
					fTick += fNextTicks - fPassedTicks;

					fPassedFrames += fNextFrames;
					fPassedTicks = fNextTicks;

				} else {
					// The target frame is located within a segment.
					const double fNewTick = (fTargetFrames - fPassedFrames ) /
						fNextTickSize;

					fTick += fNewTick;
					
					// DEBUGLOG(QString( "[end] nFrame: %1, fTick: %2, nSampleRate: %3, fNextTickSize: %4, fNextTicks: %5, fNextFrames: %6, tempoMarkers[ ii -1 ]->nColumn: %7, tempoMarkers[ ii -1 ]->fBpm: %8, fPassedTicks: %9, fPassedFrames: %10, fNewTick (tick increment): %11, fNewTick * fNextTickSize (frame increment): %12" )
					// 		 .arg( nFrame )
					// 		 .arg( fTick, 0, 'f' )
					// 		 .arg( nSampleRate )
					// 		 .arg( fNextTickSize, 0, 'f' )
					// 		 .arg( fNextTicks, 0, 'f' )
					// 		 .arg( fNextFrames, 0, 'f' )
					// 		 .arg( tempoMarkers[ ii -1 ]->nColumn )
					// 		 .arg( tempoMarkers[ ii -1 ]->fBpm )
					// 		 .arg( fPassedTicks, 0, 'f' )
					// 		 .arg( fPassedFrames, 0, 'f' )
					// 		 .arg( fNewTick, 0, 'f' )
					// 		 .arg( fNewTick * fNextTickSize, 0, 'g', 30 )
					// 		 );
											
					fPassedFrames = fTargetFrames;
					
					break;
				}
			}

			if ( fPassedFrames != fTargetFrames ) {
				// The provided nFrame is larger than the song. But,
				// luckily, we just calculated the song length in
				// frames.
				const double fSongSizeInFrames = fPassedFrames;
				const int nRepetitions = std::floor(fTargetFrames / fSongSizeInFrames);
				if ( m_fSongSizeInTicks * nRepetitions >
					 std::numeric_limits<double>::max() ) {
					ERRORLOG( QString( "Provided frames [%1] are too large." ).arg( nFrame ) );
					return 0;
				}
				fTick = m_fSongSizeInTicks * nRepetitions;

				fPassedFrames = static_cast<double>(nRepetitions) *
					fSongSizeInFrames;
				fPassedTicks = 0;

				// DEBUGLOG( QString( "[repeat] frames covered: %1, frames remaining: %2, ticks covered: %3,  nRepetitions: %4, fSongSizeInFrames: %5" )
				// 		  .arg( fPassedFrames, 0, 'g', 30 )
				// 		  .arg( fTargetFrames - fPassedFrames, 0, 'g', 30 )
				// 		  .arg( fTick, 0, 'g', 30 )
				// 		  .arg( nRepetitions )
				// 		  .arg( fSongSizeInFrames, 0, 'g', 30 )
				// 		  );
				
			}
		}
	} else {
	
		// No Timeline. Constant tempo/tick size for the whole song.
		fTick = static_cast<double>(nFrame) / fTickSize;

		// DEBUGLOG(QString( "[no timeline] nFrame: %1, sampleRate: %2, tickSize: %3" )
		// 		 .arg( nFrame ).arg( nSampleRate ).arg( fTickSize, 0, 'f' ) );

	}
	
	return fTick;
}

void AudioEngine::clearAudioBuffers( uint32_t nFrames )
{
	QMutexLocker mx( &m_MutexOutputPointer );
	float *pBuffer_L, *pBuffer_R;

	// clear main out Left and Right
	if ( m_pAudioDriver ) {
		pBuffer_L = m_pAudioDriver->getOut_L();
		pBuffer_R = m_pAudioDriver->getOut_R();
		assert( pBuffer_L != nullptr && pBuffer_R != nullptr );
		memset( pBuffer_L, 0, nFrames * sizeof( float ) );
		memset( pBuffer_R, 0, nFrames * sizeof( float ) );
	}
	
#ifdef H2CORE_HAVE_JACK
	if ( Hydrogen::get_instance()->hasJackAudioDriver() ) {
		JackAudioDriver* pJackAudioDriver = static_cast<JackAudioDriver*>(m_pAudioDriver);
	
		if( pJackAudioDriver ) {
			pJackAudioDriver->clearPerTrackAudioBuffers( nFrames );
		}
	}
#endif

	mx.unlock();

#ifdef H2CORE_HAVE_LADSPA
	if ( getState() == State::Ready ||
		 getState() == State::Playing ||
		 getState() == State::Testing ) {
		Effects* pEffects = Effects::get_instance();
		for ( unsigned i = 0; i < MAX_FX; ++i ) {	// clear FX buffers
			LadspaFX* pFX = pEffects->getLadspaFX( i );
			if ( pFX ) {
				assert( pFX->m_pBuffer_L );
				assert( pFX->m_pBuffer_R );
				memset( pFX->m_pBuffer_L, 0, nFrames * sizeof( float ) );
				memset( pFX->m_pBuffer_R, 0, nFrames * sizeof( float ) );
			}
		}
	}
#endif
}

AudioOutput* AudioEngine::createAudioDriver( const QString& sDriver )
{
	INFOLOG( QString( "Creating driver [%1]" ).arg( sDriver ) );

	auto pPref = Preferences::get_instance();
	auto pHydrogen = Hydrogen::get_instance();
	auto pSong = pHydrogen->getSong();
	AudioOutput *pAudioDriver = nullptr;

	if ( sDriver == "OSS" ) {
		pAudioDriver = new OssDriver( m_AudioProcessCallback );
	} else if ( sDriver == "JACK" ) {
		pAudioDriver = new JackAudioDriver( m_AudioProcessCallback );
#ifdef H2CORE_HAVE_JACK
		if ( auto pJackDriver = dynamic_cast<JackAudioDriver*>( pAudioDriver ) ) {
			pJackDriver->setConnectDefaults(
				Preferences::get_instance()->m_bJackConnectDefaults
			);
		}
#endif
	}
	else if ( sDriver == "ALSA" ) {
		pAudioDriver = new AlsaAudioDriver( m_AudioProcessCallback );
	}
	else if ( sDriver == "PortAudio" ) {
		pAudioDriver = new PortAudioDriver( m_AudioProcessCallback );
	}
	else if ( sDriver == "CoreAudio" ) {
		pAudioDriver = new CoreAudioDriver( m_AudioProcessCallback );
	}
	else if ( sDriver == "PulseAudio" ) {
		pAudioDriver = new PulseAudioDriver( m_AudioProcessCallback );
	}
	else if ( sDriver == "Fake" ) {
		WARNINGLOG( "*** Using FAKE audio driver ***" );
		pAudioDriver = new FakeDriver( m_AudioProcessCallback );
	}
	else if ( sDriver == "DiskWriterDriver" ) {
		pAudioDriver = new DiskWriterDriver( m_AudioProcessCallback );
	}
	else if ( sDriver == "NullDriver" ) {
		pAudioDriver = new NullDriver( m_AudioProcessCallback );
	}
	else {
		ERRORLOG( QString( "Unknown driver [%1]" ).arg( sDriver ) );
		raiseError( Hydrogen::UNKNOWN_DRIVER );
		return nullptr;
	}

	if ( pAudioDriver == nullptr ) {
		INFOLOG( QString( "Unable to create driver [%1]" ).arg( sDriver ) );
		return nullptr;
	}

	// Initialize the audio driver
	int nRes = pAudioDriver->init( pPref->m_nBufferSize );
	if ( nRes != 0 ) {
		ERRORLOG( QString( "Error code [%2] while initializing audio driver [%1]." )
				  .arg( sDriver ).arg( nRes ) );
		delete pAudioDriver;
		return nullptr;
	}

	this->lock( RIGHT_HERE );
	QMutexLocker mx(&m_MutexOutputPointer);

	// Some audio drivers require to be already registered in the
	// AudioEngine while being connected.
	m_pAudioDriver = pAudioDriver;

	// change the current audio engine state
	if ( pSong != nullptr ) {
		setState( State::Ready );
	} else {
		setState( State::Prepared );
	}

	// Unlocking earlier might execute the jack process() callback before we
	// are fully initialized.
	mx.unlock();
	this->unlock();
	
	nRes = m_pAudioDriver->connect();
	if ( nRes != 0 ) {
		raiseError( Hydrogen::ERROR_STARTING_DRIVER );
		ERRORLOG( QString( "Error code [%2] while connecting audio driver [%1]." )
				  .arg( sDriver ).arg( nRes ) );

		this->lock( RIGHT_HERE );
		mx.relock();
		
		delete m_pAudioDriver;
		m_pAudioDriver = nullptr;
		
		mx.unlock();
		this->unlock();

		return nullptr;
	}

	if ( pSong != nullptr && pHydrogen->hasJackAudioDriver() ) {
		pHydrogen->renameJackPorts( pSong );
	}
		
	setupLadspaFX();

	if ( pSong != nullptr ) {
		handleDriverChange();
	}

	EventQueue::get_instance()->push_event( EVENT_DRIVER_CHANGED, 0 );

	return pAudioDriver;
}

void AudioEngine::startAudioDrivers()
{
	INFOLOG("");
	Preferences *pPref = Preferences::get_instance();
	
	// check current state
	if ( getState() != State::Initialized ) {
		ERRORLOG( QString( "Audio engine is not in State::Initialized but [%1]" )
				  .arg( static_cast<int>( getState() ) ) );
		return;
	}

	if ( m_pAudioDriver ) {	// check if the audio m_pAudioDriver is still alive
		ERRORLOG( "The audio driver is still alive" );
	}
	if ( m_pMidiDriver ) {	// check if midi driver is still alive
		ERRORLOG( "The MIDI driver is still active" );
	}

	QString sAudioDriver = pPref->m_sAudioDriver;
#if defined(WIN32)
	QStringList drivers = { "PortAudio", "JACK" };
#elif defined(__APPLE__)
    QStringList drivers = { "CoreAudio", "JACK", "PulseAudio", "PortAudio" };
#else /* Linux */
    QStringList drivers = { "JACK", "ALSA", "OSS", "PulseAudio", "PortAudio" };
#endif

	if ( sAudioDriver != "Auto" ) {
		drivers.clear();
		drivers << sAudioDriver;
	}
	AudioOutput* pAudioDriver;
	for ( QString sDriver : drivers ) {
		if ( ( pAudioDriver = createAudioDriver( sDriver ) ) != nullptr ) {
			break;
		}
	}

	// If the audio driver could not be created, we resort to the
	// NullDriver.
	if ( m_pAudioDriver == nullptr ) {
		ERRORLOG( QString( "Couldn't start audio driver [%1], falling back to NullDriver" )
				  .arg( sAudioDriver ) );
		createAudioDriver( "NullDriver" );
	}

	// Lock both the AudioEngine and the audio output buffers.
	this->lock( RIGHT_HERE );
	QMutexLocker mx(&m_MutexOutputPointer);
	
	if ( pPref->m_sMidiDriver == "ALSA" ) {
#ifdef H2CORE_HAVE_ALSA
		// Create MIDI driver
		AlsaMidiDriver *alsaMidiDriver = new AlsaMidiDriver();
		m_pMidiDriverOut = alsaMidiDriver;
		m_pMidiDriver = alsaMidiDriver;
		m_pMidiDriver->open();
		m_pMidiDriver->setActive( true );
#endif
	} else if ( pPref->m_sMidiDriver == "PortMidi" ) {
#ifdef H2CORE_HAVE_PORTMIDI
		PortMidiDriver* pPortMidiDriver = new PortMidiDriver();
		m_pMidiDriver = pPortMidiDriver;
		m_pMidiDriverOut = pPortMidiDriver;
		m_pMidiDriver->open();
		m_pMidiDriver->setActive( true );
#endif
	} else if ( pPref->m_sMidiDriver == "CoreMIDI" ) {
#ifdef H2CORE_HAVE_COREMIDI
		CoreMidiDriver *coreMidiDriver = new CoreMidiDriver();
		m_pMidiDriver = coreMidiDriver;
		m_pMidiDriverOut = coreMidiDriver;
		m_pMidiDriver->open();
		m_pMidiDriver->setActive( true );
#endif
	} else if ( pPref->m_sMidiDriver == "JACK-MIDI" ) {
#ifdef H2CORE_HAVE_JACK
		JackMidiDriver *jackMidiDriver = new JackMidiDriver();
		m_pMidiDriverOut = jackMidiDriver;
		m_pMidiDriver = jackMidiDriver;
		m_pMidiDriver->open();
		m_pMidiDriver->setActive( true );
#endif
	}
	
	mx.unlock();
	this->unlock();
}

void AudioEngine::stopAudioDrivers()
{
	INFOLOG( "" );

	// check current state
	if ( m_state == State::Playing ) {
		this->stopPlayback(); 
	}

	if ( ( m_state != State::Prepared )
		 && ( m_state != State::Ready ) ) {
		ERRORLOG( QString( "Audio engine is not in State::Prepared or State::Ready but [%1]" )
				  .arg( static_cast<int>(m_state) ) );
		return;
	}

	this->lock( RIGHT_HERE );

	// change the current audio engine state
	setState( State::Initialized );

	// delete MIDI driver
	if ( m_pMidiDriver != nullptr ) {
		m_pMidiDriver->close();
		delete m_pMidiDriver;
		m_pMidiDriver = nullptr;
		m_pMidiDriverOut = nullptr;
	}

	// delete audio driver
	if ( m_pAudioDriver != nullptr ) {
		m_pAudioDriver->disconnect();
		QMutexLocker mx( &m_MutexOutputPointer );
		delete m_pAudioDriver;
		m_pAudioDriver = nullptr;
		mx.unlock();
	}

	this->unlock();
}

/** 
 * Restart all audio and midi drivers.
 */
void AudioEngine::restartAudioDrivers()
{
	if ( m_pAudioDriver != nullptr ) {
		stopAudioDrivers();
	}
	startAudioDrivers();
}

void AudioEngine::handleDriverChange() {

	if ( Hydrogen::get_instance()->getSong() == nullptr ) {
		WARNINGLOG( "no song set yet" );
		return;
	}
	
	handleTimelineChange();
}

float AudioEngine::getBpmAtColumn( int nColumn ) {

	auto pHydrogen = Hydrogen::get_instance();
	auto pAudioEngine = pHydrogen->getAudioEngine();

	float fBpm = pAudioEngine->getBpm();

	// Check for a change in the current BPM.
	if ( pHydrogen->getJackTimebaseState() == JackAudioDriver::Timebase::Slave &&
		 pHydrogen->getMode() == Song::Mode::Song ) {
		// Hydrogen is using the BPM broadcast by the JACK
		// server. This one does solely depend on external
		// applications and will NOT be stored in the Song.
		float fJackMasterBpm = pHydrogen->getMasterBpm();
		if ( ! std::isnan( fJackMasterBpm ) && fBpm != fJackMasterBpm ) {
			fBpm = fJackMasterBpm;
			// DEBUGLOG( QString( "Tempo update by the JACK server [%1]").arg( fJackMasterBpm ) );
		}
	} else if ( pHydrogen->getSong()->getIsTimelineActivated() &&
				pHydrogen->getMode() == Song::Mode::Song ) {

		float fTimelineBpm = pHydrogen->getTimeline()->getTempoAtColumn( nColumn );
		if ( fTimelineBpm != fBpm ) {
			// DEBUGLOG( QString( "Set tempo to timeline value [%1]").arg( fTimelineBpm ) );
			fBpm = fTimelineBpm;
		}

	} else {
		// Change in speed due to user interaction with the BPM widget
		// or corresponding MIDI or OSC events.
		if ( pAudioEngine->getNextBpm() != fBpm ) {
			// DEBUGLOG( QString( "BPM changed via Widget, OSC, or MIDI from [%1] to [%2]." )
			// 		  .arg( fBpm ).arg( pAudioEngine->getNextBpm() ) );
			fBpm = pAudioEngine->getNextBpm();
		}
	}
	return fBpm;
}

void AudioEngine::setupLadspaFX()
{
	Hydrogen* pHydrogen = Hydrogen::get_instance();
	std::shared_ptr<Song> pSong = pHydrogen->getSong();
	if ( ! pSong ) {
		return;
	}

#ifdef H2CORE_HAVE_LADSPA
	for ( unsigned nFX = 0; nFX < MAX_FX; ++nFX ) {
		LadspaFX *pFX = Effects::get_instance()->getLadspaFX( nFX );
		if ( pFX == nullptr ) {
			return;
		}

		pFX->deactivate();

		Effects::get_instance()->getLadspaFX( nFX )->connectAudioPorts(
					pFX->m_pBuffer_L,
					pFX->m_pBuffer_R,
					pFX->m_pBuffer_L,
					pFX->m_pBuffer_R
					);
		pFX->activate();
	}
#endif
}

void AudioEngine::raiseError( unsigned nErrorCode )
{
	m_pEventQueue->push_event( EVENT_ERROR, nErrorCode );
}

void AudioEngine::handleSelectedPattern() {
	// Expects the AudioEngine being locked.
	
	auto pHydrogen = Hydrogen::get_instance();
	auto pSong = pHydrogen->getSong();
	if ( pHydrogen->isPatternEditorLocked() &&
		 ( m_state == State::Playing ||
		   m_state == State::Testing ) ) {

		int nColumn = m_nColumn;
		if ( m_nColumn == -1 ) {
			nColumn = 0;
		}

		auto pPatternList = pSong->getPatternList();
		auto pColumn = ( *pSong->getPatternGroupVector() )[ nColumn ];
		
		int nPatternNumber = -1;

		int nIndex;
		for ( const auto& pattern : *pColumn ) {
			nIndex = pPatternList->index( pattern );

			if ( nIndex > nPatternNumber ) {
				nPatternNumber = nIndex;
			}
		}

		pHydrogen->setSelectedPatternNumber( nPatternNumber, true );
	}
}

void AudioEngine::processPlayNotes( unsigned long nframes )
{
	Hydrogen* pHydrogen = Hydrogen::get_instance();
	std::shared_ptr<Song> pSong = pHydrogen->getSong();

	long long nFrames;
	if ( getState() == State::Playing || getState() == State::Testing ) {
		// Current transport position.
		nFrames = getFrames();
		
	} else {
		// In case the playback is stopped and all realtime events,
		// by e.g. MIDI or Hydrogen's virtual keyboard, we disregard
		// tempo changes in the Timeline and pretend the current tick
		// size is valid for all future notes.
		nFrames = getRealtimeFrames();
	}

	// reading from m_songNoteQueue
	while ( !m_songNoteQueue.empty() ) {
		Note *pNote = m_songNoteQueue.top();
		long long nNoteStartInFrames = pNote->getNoteStart();

		// DEBUGLOG( QString( "getDoubleTick(): %1, getFrames(): %2, nframes: %3, " )
		// 		  .arg( getDoubleTick() ).arg( getFrames() )
		// 		  .arg( nframes ).append( pNote->toQString( "", true ) ) );

		if ( nNoteStartInFrames <
			 nFrames + static_cast<long long>(nframes) ) {
			/* Check if the current note has probability != 1
			 * If yes remove call random function to dequeue or not the note
			 */
			float fNoteProbability = pNote->get_probability();
			if ( fNoteProbability != 1. ) {
				if ( fNoteProbability < (float) rand() / (float) RAND_MAX ) {
					m_songNoteQueue.pop();
					pNote->get_instrument()->dequeue();
					continue;
				}
			}

			if ( pSong->getHumanizeVelocityValue() != 0 ) {
				float random = pSong->getHumanizeVelocityValue() * getGaussian( 0.2 );
				pNote->set_velocity(
							pNote->get_velocity()
							+ ( random
								- ( pSong->getHumanizeVelocityValue() / 2.0 ) )
							);
				if ( pNote->get_velocity() > 1.0 ) {
					pNote->set_velocity( 1.0 );
				} else if ( pNote->get_velocity() < 0.0 ) {
					pNote->set_velocity( 0.0 );
				}
			}

			// Offset + Random Pitch ;)
			float fPitch = pNote->get_pitch() + pNote->get_instrument()->get_pitch_offset();
			/* Check if the current instrument has random pitch factor != 0.
			 * If yes add a gaussian perturbation to the pitch
			 */
			float fRandomPitchFactor = pNote->get_instrument()->get_random_pitch_factor();
			if ( fRandomPitchFactor != 0. ) {
				fPitch += getGaussian( 0.4 ) * fRandomPitchFactor;
			}
			pNote->set_pitch( fPitch );

			/*
			 * Check if the current instrument has the property "Stop-Note" set.
			 * If yes, a NoteOff note is generated automatically after each note.
			 */
			auto  noteInstrument = pNote->get_instrument();
			if ( noteInstrument->is_stop_notes() ){
				Note *pOffNote = new Note( noteInstrument,
										   0.0,
										   0.0,
										   0.0,
										   -1,
										   0 );
				pOffNote->set_note_off( true );
				pHydrogen->getAudioEngine()->getSampler()->noteOn( pOffNote );
				delete pOffNote;
			}

			m_pSampler->noteOn( pNote );
			m_songNoteQueue.pop(); // rimuovo la nota dalla lista di note
			pNote->get_instrument()->dequeue();
			// raise noteOn event
			int nInstrument = pSong->getInstrumentList()->index( pNote->get_instrument() );
			if( pNote->get_note_off() ){
				delete pNote;
			}

			// Check whether the instrument could be found.
			if ( nInstrument != -1 ) {
				m_pEventQueue->push_event( EVENT_NOTEON, nInstrument );
			}
			
			continue;
		} else {
			// this note will not be played
			break;
		}
	}
}

void AudioEngine::clearNoteQueue()
{
	// delete all copied notes in the song notes queue
	while (!m_songNoteQueue.empty()) {
		m_songNoteQueue.top()->get_instrument()->dequeue();
		delete m_songNoteQueue.top();
		m_songNoteQueue.pop();
	}

	// delete all copied notes in the midi notes queue
	for ( unsigned i = 0; i < m_midiNoteQueue.size(); ++i ) {
		delete m_midiNoteQueue[i];
	}
	m_midiNoteQueue.clear();
}

int AudioEngine::audioEngine_process( uint32_t nframes, void* /*arg*/ )
{
	AudioEngine* pAudioEngine = Hydrogen::get_instance()->getAudioEngine();
	timeval startTimeval = currentTime2();

	// Resetting all audio output buffers with zeros.
	pAudioEngine->clearAudioBuffers( nframes );

	// Calculate maximum time to wait for audio engine lock. Using the
	// last calculated processing time as an estimate of the expected
	// processing time for this frame, the amount of slack time that
	// we can afford to wait is: m_fMaxProcessTime - m_fProcessTime.

	float sampleRate = static_cast<float>(pAudioEngine->m_pAudioDriver->getSampleRate());
	pAudioEngine->m_fMaxProcessTime = 1000.0 / ( sampleRate / nframes );
	float fSlackTime = pAudioEngine->m_fMaxProcessTime - pAudioEngine->m_fProcessTime;

	// If we expect to take longer than the available time to process,
	// require immediate locking or not at all: we're bound to drop a
	// buffer anyway.
	if ( fSlackTime < 0.0 ) {
		fSlackTime = 0.0;
	}

	/*
	 * The "try_lock" was introduced for Bug #164 (Deadlock after during
	 * alsa driver shutdown). The try_lock *should* only fail in rare circumstances
	 * (like shutting down drivers). In such cases, it seems to be ok to interrupt
	 * audio processing. Returning the special return value "2" enables the disk 
	 * writer driver to repeat the processing of the current data.
	 */
	if ( !pAudioEngine->tryLockFor( std::chrono::microseconds( (int)(1000.0*fSlackTime) ),
							  RIGHT_HERE ) ) {
		___ERRORLOG( QString( "Failed to lock audioEngine in allowed %1 ms, missed buffer" ).arg( fSlackTime ) );

		if ( dynamic_cast<DiskWriterDriver*>(pAudioEngine->m_pAudioDriver) != nullptr ) {
			return 2;	// inform the caller that we could not acquire the lock
		}

		return 0;
	}

	if ( ! ( pAudioEngine->getState() == AudioEngine::State::Ready ||
			 pAudioEngine->getState() == AudioEngine::State::Playing ) ) {
		pAudioEngine->unlock();
		return 0;
	}

	Hydrogen* pHydrogen = Hydrogen::get_instance();
	std::shared_ptr<Song> pSong = pHydrogen->getSong();
	assert( pSong );

	// Sync transport with server (in case the current audio driver is
	// designed that way)
#ifdef H2CORE_HAVE_JACK
	if ( Hydrogen::get_instance()->hasJackTransport() ) {
		// Compares the current transport state, speed in bpm, and
		// transport position with a query request to the JACK
		// server. It will only overwrite the transport state, if
		// the transport position was changed by the user by
		// e.g. clicking on the timeline.
		static_cast<JackAudioDriver*>( pHydrogen->getAudioOutput() )->updateTransportInfo();
	}
#endif

	// Check whether the tempo was changed.
	pAudioEngine->updateBpmAndTickSize();

	// Update the state of the audio engine depending on whether it
	// was started or stopped by the user.
	if ( pAudioEngine->getNextState() == State::Playing ) {
		if ( pAudioEngine->getState() == State::Ready ) {
			pAudioEngine->startPlayback();
		}
		
		pAudioEngine->setRealtimeFrames( pAudioEngine->getFrames() );
	} else {
		if ( pAudioEngine->getState() == State::Playing ) {
			pAudioEngine->stopPlayback();
		}
		
		// go ahead and increment the realtimeframes by nFrames
		// to support our realtime keyboard and midi event timing
		pAudioEngine->setRealtimeFrames( pAudioEngine->getRealtimeFrames() +
										 static_cast<long long>(nframes) );
	}

	// always update note queue.. could come from pattern or realtime input
	// (midi, keyboard)
	int nResNoteQueue = pAudioEngine->updateNoteQueue( nframes );
	if ( nResNoteQueue == -1 ) {	// end of song
		___INFOLOG( "End of song received" );
		pAudioEngine->stop();
		pAudioEngine->stopPlayback();
		pAudioEngine->locate( 0 );

		if ( dynamic_cast<FakeDriver*>(pAudioEngine->m_pAudioDriver) != nullptr ) {
			___INFOLOG( "End of song." );

			// TODO: This part of the code might not be reached
			// anymore.
			pAudioEngine->unlock();
			return 1;	// kill the audio AudioDriver thread
		}
	}

	pAudioEngine->processAudio( nframes );

	// increment the transport position
	if ( pAudioEngine->getState() == AudioEngine::State::Playing ) {
		pAudioEngine->incrementTransportPosition( nframes );
	}

	timeval finishTimeval = currentTime2();
	pAudioEngine->m_fProcessTime =
			( finishTimeval.tv_sec - startTimeval.tv_sec ) * 1000.0
			+ ( finishTimeval.tv_usec - startTimeval.tv_usec ) / 1000.0;
	
#ifdef CONFIG_DEBUG
	if ( pAudioEngine->m_fProcessTime > pAudioEngine->m_fMaxProcessTime ) {
		___WARNINGLOG( "" );
		___WARNINGLOG( "----XRUN----" );
		___WARNINGLOG( QString( "XRUN of %1 msec (%2 > %3)" )
					   .arg( ( pAudioEngine->m_fProcessTime - pAudioEngine->m_fMaxProcessTime ) )
					   .arg( pAudioEngine->m_fProcessTime ).arg( pAudioEngine->m_fMaxProcessTime ) );
		___WARNINGLOG( QString( "Ladspa process time = %1" ).arg( fLadspaTime ) );
		___WARNINGLOG( "------------" );
		___WARNINGLOG( "" );
		// raise xRun event
		EventQueue::get_instance()->push_event( EVENT_XRUN, -1 );
	}
#endif

	pAudioEngine->unlock();

	return 0;
}

void AudioEngine::processAudio( uint32_t nFrames ) {

	auto pSong = Hydrogen::get_instance()->getSong();

	// play all notes
	processPlayNotes( nFrames );

	float *pBuffer_L = m_pAudioDriver->getOut_L(),
		*pBuffer_R = m_pAudioDriver->getOut_R();
	assert( pBuffer_L != nullptr && pBuffer_R != nullptr );

	// SAMPLER
	getSampler()->process( nFrames, pSong );
	float* out_L = getSampler()->m_pMainOut_L;
	float* out_R = getSampler()->m_pMainOut_R;
	for ( unsigned i = 0; i < nFrames; ++i ) {
		pBuffer_L[ i ] += out_L[ i ];
		pBuffer_R[ i ] += out_R[ i ];
	}

	// SYNTH
	getSynth()->process( nFrames );
	out_L = getSynth()->m_pOut_L;
	out_R = getSynth()->m_pOut_R;
	for ( unsigned i = 0; i < nFrames; ++i ) {
		pBuffer_L[ i ] += out_L[ i ];
		pBuffer_R[ i ] += out_R[ i ];
	}

	timeval ladspaTime_start = currentTime2();

#ifdef H2CORE_HAVE_LADSPA
	// Process LADSPA FX
	for ( unsigned nFX = 0; nFX < MAX_FX; ++nFX ) {
		LadspaFX *pFX = Effects::get_instance()->getLadspaFX( nFX );
		if ( ( pFX ) && ( pFX->isEnabled() ) ) {
			pFX->processFX( nFrames );

			float *buf_L, *buf_R;
			if ( pFX->getPluginType() == LadspaFX::STEREO_FX ) {
				buf_L = pFX->m_pBuffer_L;
				buf_R = pFX->m_pBuffer_R;
			} else { // MONO FX
				buf_L = pFX->m_pBuffer_L;
				buf_R = buf_L;
			}

			for ( unsigned i = 0; i < nFrames; ++i ) {
				pBuffer_L[ i ] += buf_L[ i ];
				pBuffer_R[ i ] += buf_R[ i ];
				if ( buf_L[ i ] > m_fFXPeak_L[nFX] ) {
					m_fFXPeak_L[nFX] = buf_L[ i ];
				}

				if ( buf_R[ i ] > m_fFXPeak_R[nFX] ) {
					m_fFXPeak_R[nFX] = buf_R[ i ];
				}
			}
		}
	}
#endif
	timeval ladspaTime_end = currentTime2();
	m_fLadspaTime =
			( ladspaTime_end.tv_sec - ladspaTime_start.tv_sec ) * 1000.0
			+ ( ladspaTime_end.tv_usec - ladspaTime_start.tv_usec ) / 1000.0;

	// update master peaks
	float val_L, val_R;
	for ( unsigned i = 0; i < nFrames; ++i ) {
		val_L = pBuffer_L[i];
		val_R = pBuffer_R[i];

		if ( val_L > m_fMasterPeak_L ) {
			m_fMasterPeak_L = val_L;
		}

		if ( val_R > m_fMasterPeak_R ) {
			m_fMasterPeak_R = val_R;
		}
	}

	for ( auto component : *pSong->getComponents() ) {
		DrumkitComponent *pComponent = component.get();
		for ( unsigned i = 0; i < nFrames; ++i ) {
			float compo_val_L = pComponent->get_out_L(i);
			float compo_val_R = pComponent->get_out_R(i);

			if( compo_val_L > pComponent->get_peak_l() ) {
				pComponent->set_peak_l( compo_val_L );
			}
			if( compo_val_R > pComponent->get_peak_r() ) {
				pComponent->set_peak_r( compo_val_R );
			}
		}
	}

}

void AudioEngine::setState( AudioEngine::State state ) {
	m_state = state;
	EventQueue::get_instance()->push_event( EVENT_STATE, static_cast<int>(state) );
}

void AudioEngine::setNextBpm( float fNextBpm ) {
	if ( fNextBpm > MAX_BPM ) {
		m_fNextBpm = MAX_BPM;
		WARNINGLOG( QString( "Provided bpm %1 is too high. Assigning upper bound %2 instead" )
					.arg( fNextBpm ).arg( MAX_BPM ) );
	}
	else if ( fNextBpm < MIN_BPM ) {
		m_fNextBpm = MIN_BPM;
		WARNINGLOG( QString( "Provided bpm %1 is too low. Assigning lower bound %2 instead" )
					.arg( fNextBpm ).arg( MIN_BPM ) );
	}
	
	m_fNextBpm = fNextBpm;
}

void AudioEngine::setSong( std::shared_ptr<Song> pNewSong )
{
	INFOLOG( QString( "Set song: %1" ).arg( pNewSong->getName() ) );
	
	this->lock( RIGHT_HERE );

	// check current state
	// should be set by removeSong called earlier
	if ( getState() != State::Prepared ) {
		ERRORLOG( QString( "Error the audio engine is not in State::Prepared but [%1]" )
				  .arg( static_cast<int>( getState() ) ) );
	}

	// setup LADSPA FX
	if ( m_pAudioDriver != nullptr ) {
		setupLadspaFX();
	}

	// find the first pattern and set as current since we start in
	// pattern mode.
	if ( pNewSong->getPatternList()->size() > 0 ) {
		m_pPlayingPatterns->add( pNewSong->getPatternList()->get( 0 ) );
		m_nPatternSize = m_pPlayingPatterns->longest_pattern_length();
	} else {
		m_nPatternSize = MAX_NOTES;
	}

	Hydrogen::get_instance()->renameJackPorts( pNewSong );
	m_fSongSizeInTicks = static_cast<double>( pNewSong->lengthInTicks() );

	// change the current audio engine state
	setState( State::Ready );

	setNextBpm( pNewSong->getBpm() );
	// Will adapt the audio engine to the song's BPM.
	locate( 0 );

	Hydrogen::get_instance()->setTimeline( pNewSong->getTimeline() );
	Hydrogen::get_instance()->getTimeline()->activate();

	this->unlock();
}

void AudioEngine::removeSong()
{
	this->lock( RIGHT_HERE );

	if ( getState() == State::Playing ) {
		stop();
		this->stopPlayback();
	}

	// check current state
	if ( getState() != State::Ready ) {
		ERRORLOG( QString( "Error the audio engine is not in State::Ready but [%1]" )
				  .arg( static_cast<int>( getState() ) ) );
		this->unlock();
		return;
	}

	m_pPlayingPatterns->clear();
	m_pNextPatterns->clear();
	clearNoteQueue();
	m_pSampler->stopPlayingNotes();

	// change the current audio engine state
	setState( State::Prepared );
	this->unlock();
}

void AudioEngine::updateSongSize() {
	
	auto pHydrogen = Hydrogen::get_instance();
	auto pSong = pHydrogen->getSong();

	if ( pSong == nullptr ) {
		ERRORLOG( "No song set yet" );
		return;
	}

	if ( m_pPlayingPatterns->size() > 0 ) {
		m_nPatternSize = m_pPlayingPatterns->longest_pattern_length();
	} else {
		m_nPatternSize = MAX_NOTES;
	}
				
	EventQueue::get_instance()->push_event( EVENT_SONG_SIZE_CHANGED, 0 );

	if ( pHydrogen->getMode() == Song::Mode::Pattern ) {
		return;
	}

	bool bEndOfSongReached = false;

	const double fNewSongSizeInTicks = static_cast<double>( pSong->lengthInTicks() );

	// Strip away all repetitions when in loop mode but keep their
	// number. m_nPatternStartTick and m_nColumn are only defined
	// between 0 and m_fSongSizeInTicks.
	double fNewTick = std::fmod( getDoubleTick(), m_fSongSizeInTicks );
	const double fRepetitions =
		std::floor( getDoubleTick() / m_fSongSizeInTicks );

	const int nOldColumn = m_nColumn;

	// WARNINGLOG( QString( "[Before] getFrames(): %1, getBpm(): %2, getTickSize(): %3, m_nColumn: %4, getDoubleTick(): %5, fNewTick: %6, fRepetitions: %7. m_nPatternTickPosition: %8, m_nPatternStartTick: %9, m_fLastTickIntervalEnd: %10, m_fSongSizeInTicks: %11, fNewSongSizeInTicks: %12, m_nFrameOffset: %13, m_fTickMismatch: %14" )
	// 			.arg( getFrames() ).arg( getBpm() )
	// 			.arg( getTickSize(), 0, 'f' )
	// 			.arg( m_nColumn )
	// 			.arg( getDoubleTick(), 0, 'g', 30 )
	// 			.arg( fNewTick, 0, 'g', 30 )
	// 			.arg( fRepetitions, 0, 'f' )
	// 			.arg( m_nPatternTickPosition )
	// 			.arg( m_nPatternStartTick )
	// 			.arg( m_fLastTickIntervalEnd )
	// 			.arg( m_fSongSizeInTicks )
	// 			.arg( fNewSongSizeInTicks )
	// 			.arg( m_nFrameOffset )
	// 			.arg( m_fTickMismatch )
	// 			);

	m_fSongSizeInTicks = fNewSongSizeInTicks;

	// Expected behavior:
	// - changing any part of the song except of the pattern currently
	//   play shouldn't affect transport position
	// - the current transport position is defined as the start of
	//   column associated with the current position in tick + the
	//   current pattern tick position
	// - there shouldn't be a difference in behavior whether the song is
	//   looped or not
	// - this internal compensation in the transport position will
	//   only be propagated to external audio servers, like JACK, once
	//   a relocation takes place. This temporal loss of sync is done
	//   to avoid audible glitches when e.g. toggling a pattern or
	//   scrolling the pattern length spin boxes. A general intuition
	//   for a loss of synchronization when just changing song parts
	//   in one application can probably be expected.  
	// 
	// We strive for consistency in audio playback and make both the
	// current column/pattern and the transport position within the
	// pattern invariant in this transformation.
	const long nNewPatternStartTick = pHydrogen->getTickForColumn( m_nColumn );

	if ( nNewPatternStartTick == -1 ) {
		bEndOfSongReached = true;
	}
	
	if ( nNewPatternStartTick != m_nPatternStartTick ) {

		// DEBUGLOG( QString( "[nPatternStartTick mismatch] old: %1, new: %2" )
		// 		  .arg( m_nPatternStartTick )
		// 		  .arg( nNewPatternStartTick ) );
		
		fNewTick +=
			static_cast<double>(nNewPatternStartTick - m_nPatternStartTick);
	}
	
#ifdef H2CORE_HAVE_DEBUG
	const long nNewPatternTickPosition =
		static_cast<long>(std::floor( fNewTick )) - nNewPatternStartTick;
	if ( nNewPatternTickPosition != m_nPatternTickPosition ) {
		ERRORLOG( QString( "[nPatternTickPosition mismatch] old: %1, new: %2" )
				  .arg( m_nPatternTickPosition )
				  .arg( nNewPatternTickPosition ) );
	}
#endif

	// Incorporate the looped transport again
	fNewTick += fRepetitions * fNewSongSizeInTicks;
	
	// Ensure transport state is consistent
	const long long nNewFrames = computeFrameFromTick( fNewTick, &m_fTickMismatch );

	m_nFrameOffset = nNewFrames - getFrames() + m_nFrameOffset;
	m_fTickOffset = fNewTick - getDoubleTick();

	// Small rounding noise introduced in the calculation might spoil
	// things as we floor the resulting tick offset later on. Hence,
	// we round it to a specific precision.
	m_fTickOffset *= 1e8;
	m_fTickOffset = std::round( m_fTickOffset );
	m_fTickOffset *= 1e-8;
		
	// INFOLOG(QString( "[update] nNewFrame: %1, getFrames() (old): %2, m_nFrameOffset: %3, fNewTick: %4, getDoubleTick() (old): %5, m_fTickOffset : %6, tick offset (without rounding): %7, fNewSongSizeInTicks: %8, fRepetitions: %9")
	// 		.arg( nNewFrames )
	// 		.arg( getFrames() )
	// 		.arg( m_nFrameOffset )
	// 		.arg( fNewTick, 0, 'g', 30 )
	// 		.arg( getDoubleTick(), 0, 'g', 30 )
	// 		.arg( m_fTickOffset, 0, 'g', 30 )
	// 		.arg( fNewTick - getDoubleTick(), 0, 'g', 30 )
	// 		.arg( fNewSongSizeInTicks, 0, 'g', 30 )
	// 		.arg( fRepetitions, 0, 'g', 30 )
	// 		);

	setFrames( nNewFrames );
	setTick( fNewTick );

	m_fLastTickIntervalEnd += m_fTickOffset;

	// Moves all notes currently processed by Hydrogen with respect to
	// the offsets calculated above.
	handleSongSizeChange();

	// After tick and frame information as well as notes are updated
	// we will make the remainder of the transport information
	// consistent.
	updateTransportPosition( getDoubleTick() );

	// Edge case: the previous column was beyond the new song
	// end. This can e.g. happen if there are empty patterns in front
	// of a final grid cell, transport is within an empty pattern, and
	// the final grid cell get's deactivated.
	// We use all code above to ensure things are consistent but
	// locate to the beginning of the song as this might be the most
	// obvious thing to do from the user perspective.
	if ( nOldColumn >= pSong->getPatternGroupVector()->size() ) {
		// DEBUGLOG( QString( "Old column [%1] larger than new song size [%2] (in columns). Relocating to start." )
		// 		  .arg( nOldColumn )
		// 		  .arg( pSong->getPatternGroupVector()->size() ) );
		locate( 0 );
	} 
#ifdef H2CORE_HAVE_DEBUG
	else if ( nOldColumn != m_nColumn ) {
		ERRORLOG( QString( "[nColumn mismatch] old: %1, new: %2" )
				  .arg( nOldColumn )
				  .arg( m_nColumn ) );
	}
#endif
	
	if ( m_nColumn == -1 ||
		 ( bEndOfSongReached &&
		   pSong->getLoopMode() != Song::LoopMode::Enabled ) ) {
		stop();
		stopPlayback();
		locate( 0 );
	}

	// WARNINGLOG( QString( "[After] getFrames(): %1, getBpm(): %2, getTickSize(): %3, m_nColumn: %4, getDoubleTick(): %5, fNewTick: %6, fRepetitions: %7. m_nPatternTickPosition: %8, m_nPatternStartTick: %9, m_fLastTickIntervalEnd: %10, m_fSongSizeInTicks: %11, fNewSongSizeInTicks: %12, m_nFrameOffset: %13, m_fTickMismatch: %14" )
	// 			.arg( getFrames() ).arg( getBpm() )
	// 			.arg( getTickSize(), 0, 'f' )
	// 			.arg( m_nColumn )
	// 			.arg( getDoubleTick(), 0, 'g', 30 )
	// 			.arg( fNewTick, 0, 'g', 30 )
	// 			.arg( fRepetitions, 0, 'f' )
	// 			.arg( m_nPatternTickPosition )
	// 			.arg( m_nPatternStartTick )
	// 			.arg( m_fLastTickIntervalEnd )
	// 			.arg( m_fSongSizeInTicks )
	// 			.arg( fNewSongSizeInTicks )
	// 			.arg( m_nFrameOffset )
	// 			.arg( m_fTickMismatch )
	// 			);
	
}

void AudioEngine::removePlayingPattern( int nIndex ) {
	m_pPlayingPatterns->del( nIndex );
}

void AudioEngine::updatePlayingPatterns( int nColumn, long nTick ) {
	auto pHydrogen = Hydrogen::get_instance();
	auto pSong = pHydrogen->getSong();

	if ( pHydrogen->getMode() == Song::Mode::Song ) {
		// Called when transport enteres a new column.
		m_pPlayingPatterns->clear();

		if ( nColumn < 0 || nColumn >= pSong->getPatternGroupVector()->size() ) {
			return;
		}

		for ( const auto& ppattern : *( *( pSong->getPatternGroupVector() ) )[ nColumn ] ) {
			if ( ppattern != nullptr ) {
				m_pPlayingPatterns->add( ppattern );
				ppattern->addFlattenedVirtualPatterns( m_pPlayingPatterns );
			}
		}

		if ( m_pPlayingPatterns->size() > 0 ) {
			m_nPatternSize = m_pPlayingPatterns->longest_pattern_length();
		} else {
			m_nPatternSize = MAX_NOTES;
		}
				
		EventQueue::get_instance()->push_event( EVENT_PATTERN_CHANGED, 0 );
	}
	else if ( pHydrogen->getPatternMode() == Song::PatternMode::Selected ) {
		// Called asynchronous when a different pattern number
		// gets selected or the user switches from stacked into
		// selected pattern mode.
			
		auto pSelectedPattern =
			pSong->getPatternList()->get( pHydrogen->getSelectedPatternNumber() );
		if ( m_pPlayingPatterns->size() != 1 ||
			 ( m_pPlayingPatterns->size() == 1 &&
			   m_pPlayingPatterns->get( 0 ) != pSelectedPattern ) ) {

			m_pPlayingPatterns->clear();
				
			if ( pSelectedPattern != nullptr ) {
				m_pPlayingPatterns->add( pSelectedPattern );
				pSelectedPattern->addFlattenedVirtualPatterns( m_pPlayingPatterns );
			}
				
			if ( m_pPlayingPatterns->size() > 0 ) {
				m_nPatternSize = m_pPlayingPatterns->longest_pattern_length();
			} else {
				m_nPatternSize = MAX_NOTES;
			}
				
			EventQueue::get_instance()->push_event( EVENT_PATTERN_CHANGED, 0 );
		}
	}
	else if ( pHydrogen->getPatternMode() == Song::PatternMode::Stacked ) {

		if ( m_pNextPatterns->size() > 0 ) {
				
			for ( const auto& ppattern : *m_pNextPatterns ) {
				// If provided pattern is not part of the
				// list, a nullptr will be returned. Else, a
				// pointer to the deleted pattern will be
				// returned.
				if ( ppattern == nullptr ) {
					continue;
				}

				if ( ( m_pPlayingPatterns->del( ppattern ) ) == nullptr ) {
					// pPattern was not present yet. It will
					// be added.
					m_pPlayingPatterns->add( ppattern );
					ppattern->addFlattenedVirtualPatterns( m_pPlayingPatterns );
				} else {
					// pPattern was already present. It will
					// be deleted.
					ppattern->removeFlattenedVirtualPatterns( m_pPlayingPatterns );
				}
				EventQueue::get_instance()->push_event( EVENT_PATTERN_CHANGED, 0 );
			}
			m_pNextPatterns->clear();
				
			if ( m_pPlayingPatterns->size() != 0 ) {
				m_nPatternSize = m_pPlayingPatterns->longest_pattern_length();
			} else {
				m_nPatternSize = MAX_NOTES;
			}
		}
	}
}

void AudioEngine::toggleNextPattern( int nPatternNumber ) {
	auto pHydrogen = Hydrogen::get_instance();
	auto pSong = pHydrogen->getSong();
	auto pPatternList = pSong->getPatternList();
	auto pPattern = pPatternList->get( nPatternNumber );
	if ( pPattern != nullptr ) {
		if ( m_pNextPatterns->del( pPattern ) == nullptr ) {
			m_pNextPatterns->add( pPattern );
		}
	}
}

void AudioEngine::clearNextPatterns() {
	m_pNextPatterns->clear();
}

void AudioEngine::flushAndAddNextPattern( int nPatternNumber ) {
	auto pHydrogen = Hydrogen::get_instance();
	auto pSong = pHydrogen->getSong();
	auto pPatternList = pSong->getPatternList();

	m_pNextPatterns->clear();
	bool bAlreadyPlaying = false;
	
	// Note: we will not perform a bound check on the provided pattern
	// number. This way the user can use the SELECT_ONLY_NEXT_PATTERN
	// MIDI or OSC command to flush all playing patterns.
	auto pRequestedPattern = pPatternList->get( nPatternNumber );
	
	for ( int ii = 0; ii < m_pPlayingPatterns->size(); ++ii ) {

		auto pPlayingPattern = m_pPlayingPatterns->get( ii );
		if ( pPlayingPattern != pRequestedPattern ) {
			m_pNextPatterns->add( pPlayingPattern );
		}
		else if ( pRequestedPattern != nullptr ) {
			bAlreadyPlaying = true;
		}
	}
	
	// Appending the requested pattern.
	if ( ! bAlreadyPlaying && pRequestedPattern != nullptr ) {
		m_pNextPatterns->add( pRequestedPattern );
	}
}

void AudioEngine::handleTimelineChange() {

	setFrames( computeFrameFromTick( getDoubleTick(), &m_fTickMismatch ) );
	updateBpmAndTickSize();

	if ( ! Hydrogen::get_instance()->isTimelineEnabled() ) {
		// In case the Timeline was turned off, the
		// handleTempoChange() function will take over and update all
		// notes currently processed.
		return;
	}

	// Recalculate the note start in frames for all notes currently
	// processed by the AudioEngine.
	if ( m_songNoteQueue.size() > 0 ) {
		std::vector<Note*> notes;
		for ( ; ! m_songNoteQueue.empty(); m_songNoteQueue.pop() ) {
			notes.push_back( m_songNoteQueue.top() );
		}

		for ( auto nnote : notes ) {
			nnote->computeNoteStart();
			m_songNoteQueue.push( nnote );
		}
	}
	
	getSampler()->handleTimelineOrTempoChange();
}

void AudioEngine::handleTempoChange() {
	if ( m_songNoteQueue.size() == 0 ) {
		return;
	}

	// All notes share the same ticksize state (or things have gone
	// wrong at some point).
	if ( m_songNoteQueue.top()->getUsedTickSize() !=
		 getTickSize() ) {

		std::vector<Note*> notes;
		for ( ; ! m_songNoteQueue.empty(); m_songNoteQueue.pop() ) {
			notes.push_back( m_songNoteQueue.top() );
		}

		// All notes share the same ticksize state (or things have gone
		// wrong at some point).
		for ( auto nnote : notes ) {
			nnote->computeNoteStart();
			m_songNoteQueue.push( nnote );
		}
	
		getSampler()->handleTimelineOrTempoChange();
	}
}

void AudioEngine::handleSongSizeChange() {
	if ( m_songNoteQueue.size() == 0 ) {
		return;
	}

	std::vector<Note*> notes;
	for ( ; ! m_songNoteQueue.empty(); m_songNoteQueue.pop() ) {
		notes.push_back( m_songNoteQueue.top() );
	}

	for ( auto nnote : notes ) {

		// DEBUGLOG( QString( "name: %1, pos: %2, new pos: %3, tick offset: %4, tick offset floored: %5" )
		// 		  .arg( nnote->get_instrument()->get_name() )
		// 		  .arg( nnote->get_position() )
		// 		  .arg( std::max( nnote->get_position() +
		// 							   static_cast<long>(std::floor(getTickOffset())),
		// 						  static_cast<long>(0) ) )
		// 		  .arg( getTickOffset(), 0, 'f' )
		// 		  .arg( std::floor(getTickOffset()) ) );
		
		nnote->set_position( std::max( nnote->get_position() +
									   static_cast<long>(std::floor(getTickOffset())),
									   static_cast<long>(0) ) );
		nnote->computeNoteStart();
		m_songNoteQueue.push( nnote );
	}
	
	getSampler()->handleSongSizeChange();
}

long long AudioEngine::computeTickInterval( double* fTickStart, double* fTickEnd, unsigned nIntervalLengthInFrames ) {

	const auto pHydrogen = Hydrogen::get_instance();
	const auto pTimeline = pHydrogen->getTimeline();

	long long nFrameStart, nFrameEnd;

	if ( getState() == State::Ready ) {
		// This case handles all realtime events, like MIDI input or
		// Hydrogen's virtual keyboard strokes, while playback is
		// stopped. We disregard tempo changes in the Timeline and
		// pretend the current tick size is valid for all future
		// notes.
		nFrameStart = getRealtimeFrames();
	} else {
		// Enters here both when transport is rolling and
		// State::Playing is set as well as with State::Prepared
		// during testing.
		nFrameStart = getFrames();
	}
	
	// We don't use the getLookaheadInFrames() function directly
	// because the lookahead contains both a frame-based and a
	// tick-based component and would be twice as expensive to
	// calculate using the mentioned call.
	const long long nLeadLagFactor = getLeadLagInFrames( getDoubleTick() );
	const long long nLookahead = nLeadLagFactor +
		AudioEngine::nMaxTimeHumanize + 1;

	nFrameEnd = nFrameStart + nLookahead +
		static_cast<long long>(nIntervalLengthInFrames);

	if ( m_fLastTickIntervalEnd != -1 ) {
		nFrameStart += nLookahead;
	}
	
	*fTickStart = computeTickFromFrame( nFrameStart ) + m_fTickMismatch;
	*fTickEnd = computeTickFromFrame( nFrameEnd ) + m_fTickMismatch;

	// INFOLOG( QString( "nFrameStart: %1, nFrameEnd: %2, fTickStart: %3, fTickEnd: %4, m_fTickMismatch: %5" )
	// 		 .arg( nFrameStart )
	// 		 .arg( nFrameEnd )
	// 		 .arg( *fTickStart, 0, 'f' )
	// 		 .arg( *fTickEnd, 0, 'f' )
	// 		 .arg( m_fTickMismatch, 0, 'f' )
	// 		 );

	if ( getState() == State::Playing || getState() == State::Testing ) {
		// If there was a change in ticksize, account for the last used
		// lookahead to ensure the tick intervals are aligned.
		if ( m_fLastTickIntervalEnd != -1 &&
			 m_fLastTickIntervalEnd != *fTickStart ) {
			if ( m_fLastTickIntervalEnd > *fTickEnd ) {
				// The last lookahead was larger than the end of the
				// current interval would reach. We will remain at the
				// former interval end until the lookahead was eaten up in
				// future calls to updateNoteQueue() to not produce
				// glitches by non-aligned tick intervals.
				*fTickStart = m_fLastTickIntervalEnd;
				*fTickEnd = m_fLastTickIntervalEnd;
			} else {
				*fTickStart = m_fLastTickIntervalEnd;
			}
		}

		// DEBUGLOG( QString( "tick: [%1,%2], curr tick: %5, curr frame: %4, nIntervalLengthFrames: %3, realtime: %6, m_fTickOffset: %7, ticksize: %8, leadlag: %9, nlookahead: %10, m_fLastTickIntervalEnd: %11" )
		// 		  .arg( *fTickStart, 0, 'f' )
		// 		  .arg( *fTickEnd, 0, 'f' )
		// 		  .arg( nIntervalLengthInFrames )
		// 		  .arg( getFrames() )
		// 		  .arg( getDoubleTick(), 0, 'f' )
		// 		  .arg( getRealtimeFrames() )
		// 		  .arg( m_fTickOffset, 0, 'f' )
		// 		  .arg( getTickSize(), 0, 'f' )
		// 		  .arg( nLeadLagFactor )
		// 		  .arg( nLookahead )
		// 		  .arg( m_fLastTickIntervalEnd, 0, 'f' )
		// 		  );

		if ( m_fLastTickIntervalEnd < *fTickEnd ) {
			m_fLastTickIntervalEnd = *fTickEnd;
		}
	}

	return nLeadLagFactor;
}

int AudioEngine::updateNoteQueue( unsigned nIntervalLengthInFrames )
{
	Hydrogen* pHydrogen = Hydrogen::get_instance();
	std::shared_ptr<Song> pSong = pHydrogen->getSong();

	double fTickStart, fTickEnd;

	long long nLeadLagFactor =
		computeTickInterval( &fTickStart, &fTickEnd, nIntervalLengthInFrames );

	// Get initial timestamp for first tick
	gettimeofday( &m_currentTickTime, nullptr );
		
	// MIDI events now get put into the `m_songNoteQueue` as well,
	// based on their timestamp (which is given in terms of its
	// transport position and not in terms of the date-time as above).
	while ( m_midiNoteQueue.size() > 0 ) {
		Note *pNote = m_midiNoteQueue[0];

		// DEBUGLOG( QString( "getDoubleTick(): %1, getFrames(): %2, " )
		// 		  .arg( getDoubleTick() ).arg( getFrames() )
		// 		  .append( pNote->toQString( "", true ) ) );
		
		if ( pNote->get_position() >
			 static_cast<long long>(std::floor( fTickEnd )) ) {
			break;
		}

		m_midiNoteQueue.pop_front();
		pNote->get_instrument()->enqueue();
		pNote->computeNoteStart();
		m_songNoteQueue.push( pNote );
	}

	if ( getState() != State::Playing && getState() != State::Testing ) {
		// only keep going if we're playing
		return 0;
	}

	long long nNoteStart;
	float fUsedTickSize;

	double fTickMismatch;

	AutomationPath* pAutomationPath = pSong->getVelocityAutomationPath();
 
	// DEBUGLOG( QString( "tick interval: [%1 : %2], curr tick: %3, curr frame: %4")
	// 		  .arg( fTickStart, 0, 'f' ).arg( fTickEnd, 0, 'f' )
	// 		  .arg( getDoubleTick(), 0, 'f' ).arg( getFrames() ) );

	// We loop over integer ticks to ensure that all notes encountered
	// between two iterations belong to the same pattern.
	for ( long nnTick = static_cast<long>(std::floor(fTickStart));
		  nnTick < static_cast<long>(std::floor(fTickEnd)); nnTick++ ) {
		
		//////////////////////////////////////////////////////////////
		// SONG MODE
		if ( pHydrogen->getMode() == Song::Mode::Song ) {
			if ( pSong->getPatternGroupVector()->size() == 0 ) {
				// there's no song!!
				ERRORLOG( "no patterns in song." );
				stop();
				return -1;
			}
			
			int nOldColumn = m_nColumn;

			updateSongTransportPosition( static_cast<double>(nnTick) );

			// If no pattern list could not be found, either choose
			// the first one if loop mode is activate or the
			// function returns indicating that the end of the song is
			// reached.
			if ( m_nColumn == -1 ||
				 ( pSong->getLoopMode() == Song::LoopMode::Finishing &&
				   m_nColumn < nOldColumn ) ) {
				INFOLOG( "End of Song" );

				if( pHydrogen->getMidiOutput() != nullptr ){
					pHydrogen->getMidiOutput()->handleQueueAllNoteOff();
				}

				return -1;
			}
		}
		
		//////////////////////////////////////////////////////////////
		// PATTERN MODE
		else if ( pHydrogen->getMode() == Song::Mode::Pattern )	{

			updatePatternTransportPosition( nnTick );

			// DEBUGLOG( QString( "[post] nnTick: %1, m_nPatternTickPosition: %2, m_nPatternStartTick: %3, m_nPatternSize: %4" )
			// 		  .arg( nnTick ).arg( m_nPatternTickPosition )
			// 		  .arg( m_nPatternStartTick ).arg( m_nPatternSize ) );

		}
		
		//////////////////////////////////////////////////////////////
		// Metronome
		// Only trigger the metronome at a predefined rate.
		if ( m_nPatternTickPosition % 48 == 0 ) {
			float fPitch;
			float fVelocity;
			
			// Depending on whether the metronome beat will be issued
			// at the beginning or in the remainder of the pattern,
			// two different sounds and events will be used.
			if ( m_nPatternTickPosition == 0 ) {
				fPitch = 3;
				fVelocity = 1.0;
				EventQueue::get_instance()->push_event( EVENT_METRONOME, 1 );
			} else {
				fPitch = 0;
				fVelocity = 0.8;
				EventQueue::get_instance()->push_event( EVENT_METRONOME, 0 );
			}
			
			// Only trigger the sounds if the user enabled the
			// metronome. 
			if ( Preferences::get_instance()->m_bUseMetronome ) {
				m_pMetronomeInstrument->set_volume(
							Preferences::get_instance()->m_fMetronomeVolume
							);
				Note *pMetronomeNote = new Note( m_pMetronomeInstrument,
												 nnTick,
												 fVelocity,
												 0.f, // pan
												 -1,
												 fPitch
												 );
				m_pMetronomeInstrument->enqueue();
				pMetronomeNote->computeNoteStart();
				m_songNoteQueue.push( pMetronomeNote );
			}
		}

		//////////////////////////////////////////////////////////////
		// Update the notes queue.
		//
		// Supporting ticks with float precision:
		// - make FOREACH_NOTE_CST_IT_BOUND loop over all notes
		// `(_it)->first >= (_bound) && (_it)->first < (_bound + 1)`
		// - add remainder of pNote->get_position() % 1 when setting
		// nnTick as new position.
		//
		if ( m_pPlayingPatterns->size() != 0 ) {
			for ( unsigned nPat = 0 ;
				  nPat < m_pPlayingPatterns->size() ;
				  ++nPat ) {
				Pattern *pPattern = m_pPlayingPatterns->get( nPat );
				assert( pPattern != nullptr );
				Pattern::notes_t* notes = (Pattern::notes_t*)pPattern->get_notes();

				// Loop over all notes at tick nPatternTickPosition
				// (associated tick is determined by Note::__position
				// at the time of insertion into the Pattern).
				FOREACH_NOTE_CST_IT_BOUND(notes, it, m_nPatternTickPosition) {
					Note *pNote = it->second;
					if ( pNote ) {
						pNote->set_just_recorded( false );
						
						/** Time Offset in frames (relative to sample rate)
						*	Sum of 3 components: swing, humanized timing, lead_lag
						*/
						int nOffset = 0;

					   /** Swing 16ths //
						* delay the upbeat 16th-notes by a constant (manual) offset
						*/
						if ( ( ( m_nPatternTickPosition % ( MAX_NOTES / 16 ) ) == 0 )
							 && ( ( m_nPatternTickPosition % ( MAX_NOTES / 8 ) ) != 0 )
							 && pSong->getSwingFactor() > 0 ) {
							/* TODO: incorporate the factor MAX_NOTES / 32. either in Song::m_fSwingFactor
							* or make it a member variable.
							* comment by oddtime:
							* 32 depends on the fact that the swing is applied to the upbeat 16th-notes.
							* (not to upbeat 8th-notes as in jazz swing!).
							* however 32 could be changed but must be >16, otherwise the max delay is too long and
							* the swing note could be played after the next downbeat!
							*/
							// If the Timeline is activated, the tick
							// size may change at any
							// point. Therefore, the length in frames
							// of a 16-th note offset has to be
							// calculated for a particular transport
							// position and is not generally applicable.
							nOffset +=
								computeFrameFromTick( nnTick + MAX_NOTES / 32., &fTickMismatch ) *
								pSong->getSwingFactor() -
								computeFrameFromTick( nnTick, &fTickMismatch );
						}

						/* Humanize - Time parameter //
						* Add a random offset to each note. Due to
						* the nature of the Gaussian distribution,
						* the factor Song::__humanize_time_value will
						* also scale the variance of the generated
						* random variable.
						*/
						if ( pSong->getHumanizeTimeValue() != 0 ) {
							nOffset += ( int )(
										getGaussian( 0.3 )
										* pSong->getHumanizeTimeValue()
										* AudioEngine::nMaxTimeHumanize
										);
						}

						// Lead or Lag - timing parameter //
						// Add a constant offset to all notes.
						nOffset += (int) ( pNote->get_lead_lag() * nLeadLagFactor );

						// Lower bound of the offset. No note is
						// allowed to start prior to the beginning of
						// the song.
						if( nNoteStart + nOffset < 0 ){
							nOffset = -nNoteStart;
						}

						if ( nOffset > AudioEngine::nMaxTimeHumanize ) {
							nOffset = AudioEngine::nMaxTimeHumanize;
						} else if ( nOffset < -1 * AudioEngine::nMaxTimeHumanize ) {
							nOffset = -AudioEngine::nMaxTimeHumanize;
						}
						
						// Generate a copy of the current note, assign
						// it the new offset, and push it to the list
						// of all notes, which are about to be played
						// back.
						//
						// Why a copy? because it has the new offset
						// (including swing and random timing) in its
						// humanized delay, and tick position is
						// expressed referring to start time (and not
						// pattern).
						Note *pCopiedNote = new Note( pNote );
						pCopiedNote->set_humanize_delay( nOffset );

						// DEBUGLOG( QString( "getDoubleTick(): %1, getFrames(): %2, getColumn(): %3, nnTick: %4, " )
						// 		  .arg( getDoubleTick() ).arg( getFrames() )
						// 		  .arg( getColumn() ).arg( nnTick )
						// 		  .append( pCopiedNote->toQString("", true ) ) );
						
						pCopiedNote->set_position( nnTick );
						// Important: this call has to be done _after_
						// setting the position and the humanize_delay.
						pCopiedNote->computeNoteStart();
						
						if ( pHydrogen->getMode() == Song::Mode::Song ) {
							float fPos = static_cast<float>( m_nColumn ) +
								pCopiedNote->get_position() % 192 / 192.f;
							pCopiedNote->set_velocity( pNote->get_velocity() *
													   pAutomationPath->get_value( fPos ) );
						}
						pNote->get_instrument()->enqueue();
						m_songNoteQueue.push( pCopiedNote );
					}
				}
			}
		}
	}

	return 0;
}

void AudioEngine::noteOn( Note *note )
{
	// check current state
	if ( ! ( getState() == State::Playing ||
			 getState() == State::Ready ||
			 getState() == State::Testing ) ) {
		ERRORLOG( QString( "Error the audio engine is not in State::Ready, State::Playing, or State::Testing but [%1]" )
					 .arg( static_cast<int>( getState() ) ) );
		delete note;
		return;
	}

	m_midiNoteQueue.push_back( note );
}

bool AudioEngine::compare_pNotes::operator()(Note* pNote1, Note* pNote2)
{
	float fTickSize = Hydrogen::get_instance()->getAudioEngine()->getTickSize();
	return (pNote1->get_humanize_delay() +
			AudioEngine::computeFrame( pNote1->get_position(), fTickSize ) ) >
		(pNote2->get_humanize_delay() +
		 AudioEngine::computeFrame( pNote2->get_position(), fTickSize ) );
}

void AudioEngine::play() {
	
	assert( m_pAudioDriver );

#ifdef H2CORE_HAVE_JACK
	if ( Hydrogen::get_instance()->hasJackTransport() ) {
		// Tell all other JACK clients to start as well and wait for
		// the JACK server to give the signal.
		static_cast<JackAudioDriver*>( m_pAudioDriver )->startTransport();
		return;
	}
#endif

	setNextState( State::Playing );

	if ( dynamic_cast<FakeDriver*>(m_pAudioDriver) != nullptr ) {
		static_cast<FakeDriver*>( m_pAudioDriver )->processCallback();
	}
}

void AudioEngine::stop() {
	assert( m_pAudioDriver );
	
#ifdef H2CORE_HAVE_JACK
	if ( Hydrogen::get_instance()->hasJackTransport() ) {
		// Tell all other JACK clients to stop as well and wait for
		// the JACK server to give the signal.
		static_cast<JackAudioDriver*>( m_pAudioDriver )->stopTransport();
		return;
	}
#endif
	
	setNextState( State::Ready );
}

double AudioEngine::getLeadLagInTicks() {
	return 5;
}

long long AudioEngine::getLeadLagInFrames( double fTick ) {
	double fTickMismatch;
	long long nFrameStart = computeFrameFromTick( fTick, &fTickMismatch );
	long long nFrameEnd = computeFrameFromTick( fTick + AudioEngine::getLeadLagInTicks(),
						  &fTickMismatch );

	return nFrameEnd - nFrameStart;
}

long long AudioEngine::getLookaheadInFrames( double fTick ) {
	return getLeadLagInFrames( fTick ) +
		AudioEngine::nMaxTimeHumanize + 1;
}

double AudioEngine::getDoubleTick() const {
	return TransportInfo::getTick();
}

long AudioEngine::getTick() const {
	return static_cast<long>(std::floor( getDoubleTick() ));
}

bool AudioEngine::testFrameToTickConversion() {
	auto pHydrogen = Hydrogen::get_instance();
	auto pCoreActionController = pHydrogen->getCoreActionController();

	bool bNoMismatch = true;
	
	pCoreActionController->activateTimeline( true );
	pCoreActionController->addTempoMarker( 0, 120 );
	pCoreActionController->addTempoMarker( 3, 100 );
	pCoreActionController->addTempoMarker( 5, 40 );
	pCoreActionController->addTempoMarker( 7, 200 );

	double fFrameOffset1, fFrameOffset2, fFrameOffset3,
		fFrameOffset4, fFrameOffset5, fFrameOffset6;
	
	long long nFrame1 = 342732;
	long long nFrame2 = 1037223;
	long long nFrame3 = 453610333722;
	double fTick1 = computeTickFromFrame( nFrame1 );
	long long nFrame1Computed = computeFrameFromTick( fTick1, &fFrameOffset1 );
	double fTick2 = computeTickFromFrame( nFrame2 );
	long long nFrame2Computed = computeFrameFromTick( fTick2, &fFrameOffset2 );
	double fTick3 = computeTickFromFrame( nFrame3 );
	long long nFrame3Computed = computeFrameFromTick( fTick3, &fFrameOffset3 );
	
	if ( nFrame1Computed != nFrame1 || std::abs( fFrameOffset1 ) > 1e-10 ) {
		qDebug() << QString( "[testFrameToTickConversion] [1] nFrame: %1, fTick: %2, nFrameComputed: %3, fFrameOffset: %4, frame diff: %5" )
			.arg( nFrame1 ).arg( fTick1, 0, 'f' ).arg( nFrame1Computed )
			.arg( fFrameOffset1, 0, 'E', -1 )
			.arg( nFrame1Computed - nFrame1 )
			.toLocal8Bit().data();
		bNoMismatch = false;
	}
	if ( nFrame2Computed != nFrame2 || std::abs( fFrameOffset2 ) > 1e-10 ) {
		qDebug() << QString( "[testFrameToTickConversion] [2] nFrame: %1, fTick: %2, nFrameComputed: %3, fFrameOffset: %4, frame diff: %5" )
			.arg( nFrame2 ).arg( fTick2, 0, 'f' ).arg( nFrame2Computed )
			.arg( fFrameOffset2, 0, 'E', -1 )
			.arg( nFrame2Computed - nFrame2 ).toLocal8Bit().data();
		bNoMismatch = false;
	}
	if ( nFrame3Computed != nFrame3 || std::abs( fFrameOffset3 ) > 1e-6 ) {
		qDebug() << QString( "[testFrameToTickConversion] [3] nFrame: %1, fTick: %2, nFrameComputed: %3, fFrameOffset: %4, frame diff: %5" )
			.arg( nFrame3 ).arg( fTick3, 0, 'f' ).arg( nFrame3Computed )
			.arg( fFrameOffset3, 0, 'E', -1 )
			.arg( nFrame3Computed - nFrame3 ).toLocal8Bit().data();
		bNoMismatch = false;
	}

	double fTick4 = 552;
	double fTick5 = 1939;
	double fTick6 = 534623409;
	long long nFrame4 = computeFrameFromTick( fTick4, &fFrameOffset4 );
	double fTick4Computed = computeTickFromFrame( nFrame4 ) +
		fFrameOffset4;
	long long nFrame5 = computeFrameFromTick( fTick5, &fFrameOffset5 );
	double fTick5Computed = computeTickFromFrame( nFrame5 ) +
		fFrameOffset5;
	long long nFrame6 = computeFrameFromTick( fTick6, &fFrameOffset6 );
	double fTick6Computed = computeTickFromFrame( nFrame6 ) +
		fFrameOffset6;
	
	
	if ( abs( fTick4Computed - fTick4 ) > 1e-9 ) {
		qDebug() << QString( "[testFrameToTickConversion] [4] nFrame: %1, fTick: %2, fTickComputed: %3, fFrameOffset: %4, tick diff: %5" )
			.arg( nFrame4 ).arg( fTick4, 0, 'f' ).arg( fTick4Computed, 0, 'f' )
			.arg( fFrameOffset4, 0, 'E' )
			.arg( fTick4Computed - fTick4 ).toLocal8Bit().data();
		bNoMismatch = false;
	}

	if ( abs( fTick5Computed - fTick5 ) > 1e-9 ) {
		qDebug() << QString( "[testFrameToTickConversion] [5] nFrame: %1, fTick: %2, fTickComputed: %3, fFrameOffset: %4, tick diff: %5" )
			.arg( nFrame5 ).arg( fTick5, 0, 'f' ).arg( fTick5Computed, 0, 'f' )
			.arg( fFrameOffset5, 0, 'E' )
			.arg( fTick5Computed - fTick5 ).toLocal8Bit().data();
		bNoMismatch = false;
	}

	if ( abs( fTick6Computed - fTick6 ) > 1e-6 ) {
		qDebug() << QString( "[testFrameToTickConversion] [6] nFrame: %1, fTick: %2, fTickComputed: %3, fFrameOffset: %4, tick diff: %5" )
			.arg( nFrame6 ).arg( fTick6, 0, 'f' ).arg( fTick6Computed, 0, 'f' )
			.arg( fFrameOffset6, 0, 'E' )
			.arg( fTick6Computed - fTick6 ).toLocal8Bit().data();
		bNoMismatch = false;
	}

	return bNoMismatch;
}

bool AudioEngine::testTransportProcessing() {
	auto pHydrogen = Hydrogen::get_instance();
	auto pPref = Preferences::get_instance();
	auto pCoreActionController = pHydrogen->getCoreActionController();
	
	pCoreActionController->activateTimeline( false );
	pCoreActionController->activateLoopMode( true );

	lock( RIGHT_HERE );

	// Seed with a real random value, if available
    std::random_device randomSeed;
 
    // Choose a random mean between 1 and 6
    std::default_random_engine randomEngine( randomSeed() );
    std::uniform_int_distribution<int> frameDist( 1, pPref->m_nBufferSize );
	std::uniform_real_distribution<float> tempoDist( MIN_BPM, MAX_BPM );

	// For this call the AudioEngine still needs to be in state
	// Playing or Ready.
	reset( false );

	setState( AudioEngine::State::Testing );

	// Check consistency of updated frames and ticks while using a
	// random buffer size (e.g. like PulseAudio does).
	
	uint32_t nFrames;
	double fCheckTick;
	long long nCheckFrame, nLastFrame = 0;

	bool bNoMismatch = true;

	// 2112 is the number of ticks within the test song.
	int nMaxCycles =
		std::max( std::ceil( 2112.0 /
							 static_cast<float>(pPref->m_nBufferSize) *
							getTickSize() * 4.0 ),
				  2112.0 );
	int nn = 0;

	while ( getDoubleTick() < m_fSongSizeInTicks ) {

		nFrames = frameDist( randomEngine );

		incrementTransportPosition( nFrames );

		if ( ! testCheckTransportPosition( "[testTransportProcessing] constant tempo" ) ) {
			bNoMismatch = false;
			break;
		}

		if ( getFrames() - nFrames != nLastFrame ) {
			qDebug() << QString( "[testTransportProcessing] [constant tempo] inconsistent frame update. getFrames(): %1, nFrames: %2, nLastFrame: %3" )
				.arg( getFrames() ).arg( nFrames ).arg( nLastFrame );
			bNoMismatch = false;
			break;
		}
		nLastFrame = getFrames();

		nn++;

		if ( nn > nMaxCycles ) {
			qDebug() << QString( "[testTransportProcessing] [constant tempo] end of the song wasn't reached in time. getFrames(): %1, ticks: %2, getTickSize(): %3, m_fSongSizeInTicks: %4, nMaxCycles: %5" )
				.arg( getFrames() )
				.arg( getDoubleTick(), 0, 'f' )
				.arg( getTickSize(), 0, 'f' )
				.arg( m_fSongSizeInTicks, 0, 'f' )
				.arg( nMaxCycles );
			bNoMismatch = false;
			break;
		}
	}

	reset( false );
	nLastFrame = 0;

	float fBpm;
	float fLastBpm = getBpm();
	int nCyclesPerTempo = 5;
	int nPrevLastFrame = 0;

	long long nTotalFrames = 0;

	nn = 0;

	while ( getDoubleTick() < m_fSongSizeInTicks ) {

		fBpm = tempoDist( randomEngine );

		nPrevLastFrame = nLastFrame;
		nLastFrame =
			static_cast<int>(std::round( static_cast<double>(nLastFrame) *
										 static_cast<double>(fLastBpm) /
										 static_cast<double>(fBpm) ));
		
		setNextBpm( fBpm );
		updateBpmAndTickSize();
		
		for ( int cc = 0; cc < nCyclesPerTempo; ++cc ) {
			nFrames = frameDist( randomEngine );

			incrementTransportPosition( nFrames );

			if ( ! testCheckTransportPosition( "[testTransportProcessing] variable tempo" ) ) {
				setState( AudioEngine::State::Ready );
				unlock();
				return bNoMismatch;
			}

			if ( ( cc > 0 && getFrames() - nFrames != nLastFrame ) ||
				 // errors in the rescaling of nLastFrame are omitted.
				 ( cc == 0 &&
				   abs( ( getFrames() - nFrames - nLastFrame ) /
						getFrames() ) > 1e-8 ) ) {
				qDebug() << QString( "[testTransportProcessing] [variable tempo] inconsistent frame update. getFrames(): %1, nFrames: %2, nLastFrame: %3, cc: %4, fLastBpm: %5, fBpm: %6, nPrevLastFrame: %7" )
					.arg( getFrames() ).arg( nFrames )
					.arg( nLastFrame ).arg( cc )
					.arg( fLastBpm, 0, 'f' ).arg( fBpm, 0, 'f' )
					.arg( nPrevLastFrame );
				bNoMismatch = false;
				setState( AudioEngine::State::Ready );
				unlock();
				return bNoMismatch;
			}
			
			nLastFrame = getFrames();

			// Using the offset Hydrogen can keep track of the actual
			// number of frames passed since the playback was started
			// even in case a tempo change was issued by the user.
			nTotalFrames += nFrames;
			if ( getFrames() - m_nFrameOffset != nTotalFrames ) {
				qDebug() << QString( "[testTransportProcessing] [variable tempo] frame offset incorrect. getFrames(): %1, m_nFrameOffset: %2, nTotalFrames: %3" )
					.arg( getFrames() ).arg( m_nFrameOffset ).arg( nTotalFrames );
				bNoMismatch = false;
				setState( AudioEngine::State::Ready );
				unlock();
				return bNoMismatch;
			}
		}
		
		fLastBpm = fBpm;

		nn++;

		if ( nn > nMaxCycles ) {
			qDebug() << "[testTransportProcessing] [variable tempo] end of the song wasn't reached in time.";
			bNoMismatch = false;
			break;
		}
	}

	setState( AudioEngine::State::Ready );

	unlock();

	pCoreActionController->activateTimeline( true );
	pCoreActionController->addTempoMarker( 0, 120 );
	pCoreActionController->addTempoMarker( 1, 100 );
	pCoreActionController->addTempoMarker( 2, 20 );
	pCoreActionController->addTempoMarker( 3, 13.4 );
	pCoreActionController->addTempoMarker( 4, 383.2 );
	pCoreActionController->addTempoMarker( 5, 64.38372 );
	pCoreActionController->addTempoMarker( 6, 96.3 );
	pCoreActionController->addTempoMarker( 7, 240.46 );
	pCoreActionController->addTempoMarker( 8, 200.1 );

	lock( RIGHT_HERE );
	setState( AudioEngine::State::Testing );
	
	// Check consistency after switching on the Timeline
	if ( ! testCheckTransportPosition( "[testTransportProcessing] timeline: off" ) ) {
		bNoMismatch = false;
	}
	
	nn = 0;
	nLastFrame = 0;

	while ( getDoubleTick() < m_fSongSizeInTicks ) {

		nFrames = frameDist( randomEngine );

		incrementTransportPosition( nFrames );

		if ( ! testCheckTransportPosition( "[testTransportProcessing] timeline" ) ) {
			bNoMismatch = false;
			break;
		}

		if ( getFrames() - nFrames != nLastFrame ) {
			qDebug() << QString( "[testTransportProcessing] [timeline] inconsistent frame update. getFrames(): %1, nFrames: %2, nLastFrame: %3" )
				.arg( getFrames() ).arg( nFrames ).arg( nLastFrame );
			bNoMismatch = false;
			break;
		}
		nLastFrame = getFrames();

		nn++;

		if ( nn > nMaxCycles ) {
			qDebug() << "[testTransportProcessing] [timeline] end of the song wasn't reached in time.";
			bNoMismatch = false;
			break;
		}
	}

	setState( AudioEngine::State::Ready );

	unlock();

	// Check consistency after switching on the Timeline
	pCoreActionController->activateTimeline( false );

	lock( RIGHT_HERE );
	setState( AudioEngine::State::Testing );

	if ( ! testCheckTransportPosition( "[testTransportProcessing] timeline: off" ) ) {
		bNoMismatch = false;
	}

	reset( false );

	setState( AudioEngine::State::Ready );

	unlock();

	// Check consistency of playback in PatternMode
	pCoreActionController->activateSongMode( false );

	lock( RIGHT_HERE );
	setState( AudioEngine::State::Testing );

	nLastFrame = 0;
	fLastBpm = 0;
	nTotalFrames = 0;

	int nDifferentTempos = 10;

	for ( int tt = 0; tt < nDifferentTempos; ++tt ) {

		fBpm = tempoDist( randomEngine );

		nLastFrame = std::round( nLastFrame * fLastBpm / fBpm );
		
		setNextBpm( fBpm );
		updateBpmAndTickSize();

		fLastBpm = fBpm;
		
		for ( int cc = 0; cc < nCyclesPerTempo; ++cc ) {
			nFrames = frameDist( randomEngine );

			incrementTransportPosition( nFrames );

			if ( ! testCheckTransportPosition( "[testTransportProcessing] pattern mode" ) ) {
				setState( AudioEngine::State::Ready );
				unlock();
				pCoreActionController->activateSongMode( true );
				return bNoMismatch;
			}

			if ( ( cc > 0 && getFrames() - nFrames != nLastFrame ) ||
				 // errors in the rescaling of nLastFrame are omitted.
				 ( cc == 0 && abs( getFrames() - nFrames - nLastFrame ) > 1 ) ) {
				qDebug() << QString( "[testTransportProcessing] [pattern mode] inconsistent frame update. getFrames(): %1, nFrames: %2, nLastFrame: %3" )
					.arg( getFrames() ).arg( nFrames ).arg( nLastFrame );
				bNoMismatch = false;
				setState( AudioEngine::State::Ready );
				unlock();
				pCoreActionController->activateSongMode( true );
				return bNoMismatch;
			}
			
			nLastFrame = getFrames();

			// Using the offset Hydrogen can keep track of the actual
			// number of frames passed since the playback was started
			// even in case a tempo change was issued by the user.
			nTotalFrames += nFrames;
			if ( getFrames() - m_nFrameOffset != nTotalFrames ) {
				qDebug() << QString( "[testTransportProcessing] [pattern mode] frame offset incorrect. getFrames(): %1, m_nFrameOffset: %2, nTotalFrames: %3" )
					.arg( getFrames() ).arg( m_nFrameOffset ).arg( nTotalFrames );
				bNoMismatch = false;
				setState( AudioEngine::State::Ready );
				unlock();
				pCoreActionController->activateSongMode( true );
				return bNoMismatch;
			}
		}
	}

	reset( false );

	setState( AudioEngine::State::Ready );

	unlock();
	pCoreActionController->activateSongMode( true );

	return bNoMismatch;
}

bool AudioEngine::testTransportRelocation() {
	auto pHydrogen = Hydrogen::get_instance();
	auto pPref = Preferences::get_instance();

	lock( RIGHT_HERE );

	// Seed with a real random value, if available
    std::random_device randomSeed;
 
    // Choose a random mean between 1 and 6
    std::default_random_engine randomEngine( randomSeed() );
    std::uniform_real_distribution<double> tickDist( 0, m_fSongSizeInTicks );
	std::uniform_int_distribution<long long> frameDist( 0, pPref->m_nBufferSize );

	// For this call the AudioEngine still needs to be in state
	// Playing or Ready.
	reset( false );

	setState( AudioEngine::State::Testing );

	// Check consistency of updated frames and ticks while relocating
	// transport.
	double fNewTick;
	long long nNewFrame;

	bool bNoMismatch = true;

	int nProcessCycles = 100;
	for ( int nn = 0; nn < nProcessCycles; ++nn ) {

		if ( nn < nProcessCycles - 2 ) {
			fNewTick = tickDist( randomEngine );
		}
		else if ( nn < nProcessCycles - 1 ) {
			// Results in an unfortunate rounding error due to the
			// song end at 2112. 
			fNewTick = 2111.928009209;
		}
		else {
			// There was a rounding error at this particular tick.
			fNewTick = 960;
			
		}

		locate( fNewTick, false );

		if ( ! testCheckTransportPosition( "[testTransportRelocation] mismatch tick-based" ) ) {
			bNoMismatch = false;
			break;
		}

		// Frame-based relocation
		nNewFrame = frameDist( randomEngine );
		locateToFrame( nNewFrame );

		if ( ! testCheckTransportPosition( "[testTransportRelocation] mismatch frame-based" ) ) {
			bNoMismatch = false;
			break;
		}
	}

	reset( false );
	
	setState( AudioEngine::State::Ready );

	unlock();


	return bNoMismatch;
}

bool AudioEngine::testComputeTickInterval() {
	auto pHydrogen = Hydrogen::get_instance();
	auto pPref = Preferences::get_instance();

	lock( RIGHT_HERE );

	// Seed with a real random value, if available
    std::random_device randomSeed;
 
    // Choose a random mean between 1 and 6
    std::default_random_engine randomEngine( randomSeed() );
	std::uniform_real_distribution<float> frameDist( 1, pPref->m_nBufferSize );
	std::uniform_real_distribution<float> tempoDist( MIN_BPM, MAX_BPM );

	// For this call the AudioEngine still needs to be in state
	// Playing or Ready.
	reset( false );

	setState( AudioEngine::State::Testing );

	// Check consistency of tick intervals processed in
	// updateNoteQueue() (no overlap and no holes). We pretend to
	// receive transport position updates of random size (as e.g. used
	// in PulseAudio).
	
	double fTickStart, fTickEnd;
	double fLastTickStart = 0;
	double fLastTickEnd = 0;
	long long nLeadLagFactor;
	long long nLastLeadLagFactor = 0;
	int nFrames;

	bool bNoMismatch = true;

	int nProcessCycles = 100;
	for ( int nn = 0; nn < nProcessCycles; ++nn ) {

		nFrames = frameDist( randomEngine );

		nLeadLagFactor = computeTickInterval( &fTickStart, &fTickEnd,
											  nFrames );

		if ( nLastLeadLagFactor != 0 &&
			 // Since we move a region on two mismatching grids (frame
			 // and tick), it's okay if the calculated is not
			 // perfectly constant. For certain tick ranges more
			 // frames are enclosed than for others (Moire effect). 
			 std::abs( nLastLeadLagFactor - nLeadLagFactor ) > 1 ) {
			qDebug() << QString( "[testComputeTickInterval] [constant tempo] There should not be altering lead lag with constant tempo [new: %1, prev: %2].")
				.arg( nLeadLagFactor ).arg( nLastLeadLagFactor );
			bNoMismatch = false;
		}
		nLastLeadLagFactor = nLeadLagFactor;	

		if ( nn == 0 && fTickStart != 0 ){
			qDebug() << QString( "[testComputeTickInterval] [constant tempo] First interval [%1,%2] does not start at 0.")
				.arg( fTickStart, 0, 'f' ).arg( fTickEnd, 0, 'f' );
			bNoMismatch = false;
		}

		if ( fTickStart != fLastTickEnd ) {
			qDebug() << QString( "[testComputeTickInterval] [variable tempo] Interval [%1,%2] does not align with previous one [%3,%4]. nFrames: %5, curr tick: %6, curr frames: %7, bpm: %8, tick size: %9, nLeadLagFactor: %10")
				.arg( fTickStart, 0, 'f' )
				.arg( fTickEnd, 0, 'f' )
				.arg( fLastTickStart, 0, 'f' )
				.arg( fLastTickEnd, 0, 'f' )
				.arg( nFrames )
				.arg( getDoubleTick(), 0, 'f' )
				.arg( getFrames() )
				.arg( getBpm(), 0, 'f' )
				.arg( getTickSize(), 0, 'f' )
				.arg( nLeadLagFactor );
			bNoMismatch = false;
		}
		
		fLastTickStart = fTickStart;
		fLastTickEnd = fTickEnd;

		incrementTransportPosition( nFrames );
	}

	reset( false );

	fLastTickStart = 0;
	fLastTickEnd = 0;
	
	float fBpm;

	int nTempoChanges = 20;
	int nProcessCyclesPerTempo = 5;
	for ( int tt = 0; tt < nTempoChanges; ++tt ) {

		fBpm = tempoDist( randomEngine );
		setNextBpm( fBpm );
		
		for ( int cc = 0; cc < nProcessCyclesPerTempo; ++cc ) {

			nFrames = frameDist( randomEngine );

			nLeadLagFactor = computeTickInterval( &fTickStart, &fTickEnd,
												  nFrames );

			if ( cc == 0 && tt == 0 && fTickStart != 0 ){
				qDebug() << QString( "[testComputeTickInterval] [variable tempo] First interval [%1,%2] does not start at 0.")
					.arg( fTickStart, 0, 'f' )
					.arg( fTickEnd, 0, 'f' );
				bNoMismatch = false;
				break;
			}

			if ( fTickStart != fLastTickEnd ) {
				qDebug() << QString( "[variable tempo] Interval [%1,%2] does not align with previous one [%3,%4]. nFrames: %5, curr tick: %6, curr frames: %7, bpm: %8, tick size: %9, nLeadLagFactor: %10")
					.arg( fTickStart, 0, 'f' )
					.arg( fTickEnd, 0, 'f' )
					.arg( fLastTickStart, 0, 'f' )
					.arg( fLastTickEnd, 0, 'f' )
					.arg( nFrames )
					.arg( getDoubleTick(), 0, 'f' )
					.arg( getFrames() )
					.arg( getBpm(), 0, 'f' )
					.arg( getTickSize(), 0, 'f' )
					.arg( nLeadLagFactor );
				bNoMismatch = false;
				break;
			}

			fLastTickStart = fTickStart;
			fLastTickEnd = fTickEnd;

			incrementTransportPosition( nFrames );
		}

		if ( ! bNoMismatch ) {
			break;
		}
	}
	
	reset( false );

	setState( AudioEngine::State::Ready );

	unlock();


	return bNoMismatch;
}

bool AudioEngine::testSongSizeChange() {
	
	auto pHydrogen = Hydrogen::get_instance();
	auto pCoreActionController = pHydrogen->getCoreActionController();
	auto pSong = pHydrogen->getSong();

	lock( RIGHT_HERE );
	reset( false );
	setState( AudioEngine::State::Ready );

	unlock();
	pCoreActionController->locateToColumn( 4 );
	lock( RIGHT_HERE );
	setState( AudioEngine::State::Testing );

	if ( ! testToggleAndCheckConsistency( 1, 1, "[testSongSizeChange] prior" ) ) {
		setState( AudioEngine::State::Ready );
		unlock();
		return false;
	}
		
	// Toggle a grid cell after to the current transport position
	if ( ! testToggleAndCheckConsistency( 6, 6, "[testSongSizeChange] after" ) ) {
		setState( AudioEngine::State::Ready );
		unlock();
		return false;
	}

	// Now we head to the "same" position inside the song but with the
	// transport being looped once.
	int nTestColumn = 4;
	long nNextTick = pHydrogen->getTickForColumn( nTestColumn );
	if ( nNextTick == -1 ) {
		qDebug() << QString( "[testSongSizeChange] Bad test design: there is no column [%1]" )
			.arg( nTestColumn );
		setState( AudioEngine::State::Ready );
		unlock();
		return false;
	}

	nNextTick += pSong->lengthInTicks();
	
	unlock();
	pCoreActionController->activateLoopMode( true );
	pCoreActionController->locateToTick( nNextTick );
	lock( RIGHT_HERE );
	
	if ( ! testToggleAndCheckConsistency( 1, 1, "[testSongSizeChange] looped:prior" ) ) {
		setState( AudioEngine::State::Ready );
		unlock();
		pCoreActionController->activateLoopMode( false );
		return false;
	}
		
	// Toggle a grid cell after to the current transport position
	if ( ! testToggleAndCheckConsistency( 13, 6, "[testSongSizeChange] looped:after" ) ) {
		setState( AudioEngine::State::Ready );
		unlock();
		pCoreActionController->activateLoopMode( false );
		return false;
	}

	setState( AudioEngine::State::Ready );
	unlock();
	pCoreActionController->activateLoopMode( false );

	return true;
}

bool AudioEngine::testSongSizeChangeInLoopMode() {
	auto pHydrogen = Hydrogen::get_instance();
	auto pCoreActionController = pHydrogen->getCoreActionController();
	auto pPref = Preferences::get_instance();
	
	pCoreActionController->activateTimeline( false );
	pCoreActionController->activateLoopMode( true );

	lock( RIGHT_HERE );

	int nColumns = pHydrogen->getSong()->getPatternGroupVector()->size();

	// Seed with a real random value, if available
    std::random_device randomSeed;
 
    // Choose a random mean between 1 and 6
    std::default_random_engine randomEngine( randomSeed() );
    std::uniform_real_distribution<double> frameDist( 1, pPref->m_nBufferSize );
	std::uniform_int_distribution<int> columnDist( nColumns, nColumns + 100 );

	// For this call the AudioEngine still needs to be in state
	// Playing or Ready.
	reset( false );

	setState( AudioEngine::State::Testing );

	uint32_t nFrames = 500;
	double fInitialSongSize = m_fSongSizeInTicks;
	int nNewColumn;

	bool bNoMismatch = true;

	int nNumberOfTogglings = 1;

	for ( int nn = 0; nn < nNumberOfTogglings; ++nn ) {

		locate( fInitialSongSize + frameDist( randomEngine ) );

		if ( ! testCheckTransportPosition( "[testSongSizeChangeInLoopMode] relocation" ) ) {
			bNoMismatch = false;
			break;
		}

		incrementTransportPosition( nFrames );

		if ( ! testCheckTransportPosition( "[testSongSizeChangeInLoopMode] first increment" ) ) {
			bNoMismatch = false;
			break;
		}

		nNewColumn = columnDist( randomEngine );

		unlock();
		pCoreActionController->toggleGridCell( nNewColumn, 0 );
		lock( RIGHT_HERE );

		if ( ! testCheckTransportPosition( "[testSongSizeChangeInLoopMode] first toggling" ) ) {
			bNoMismatch = false;
			break;
		}

		if ( fInitialSongSize == m_fSongSizeInTicks ) {
			qDebug() << QString( "[testSongSizeChangeInLoopMode] [first toggling] no song enlargement %1")
				.arg( m_fSongSizeInTicks );
			bNoMismatch = false;
			break;
		}

		incrementTransportPosition( nFrames );

		if ( ! testCheckTransportPosition( "[testSongSizeChange] second increment" ) ) {
			bNoMismatch = false;
			break;
		}
														  
		unlock();
		pCoreActionController->toggleGridCell( nNewColumn, 0 );
		lock( RIGHT_HERE );

		if ( ! testCheckTransportPosition( "[testSongSizeChange] second toggling" ) ) {
			bNoMismatch = false;
			break;
		}
		
		if ( fInitialSongSize != m_fSongSizeInTicks ) {
			qDebug() << QString( "[testSongSizeChange] [second toggling] song size mismatch original: %1, new: %2" )
				.arg( fInitialSongSize ).arg( m_fSongSizeInTicks );
			bNoMismatch = false;
			break;
		}

		incrementTransportPosition( nFrames );

		if ( ! testCheckTransportPosition( "[testSongSizeChange] third increment" ) ) {
			bNoMismatch = false;
			break;
		}
	}

	setState( AudioEngine::State::Ready );

	unlock();

	return bNoMismatch;
}

bool AudioEngine::testNoteEnqueuing() {
	auto pHydrogen = Hydrogen::get_instance();
	auto pSong = pHydrogen->getSong();
	auto pCoreActionController = pHydrogen->getCoreActionController();
	auto pPref = Preferences::get_instance();

	pCoreActionController->activateTimeline( false );
	pCoreActionController->activateLoopMode( false );
	pCoreActionController->activateSongMode( true );
	lock( RIGHT_HERE );

	// Seed with a real random value, if available
    std::random_device randomSeed;
 
    // Choose a random mean between 1 and 6
    std::default_random_engine randomEngine( randomSeed() );
    std::uniform_int_distribution<int> frameDist( pPref->m_nBufferSize / 2,
												  pPref->m_nBufferSize );

	// For this call the AudioEngine still needs to be in state
	// Playing or Ready.
	reset( false );

	setState( AudioEngine::State::Testing );

	// Check consistency of updated frames and ticks while using a
	// random buffer size (e.g. like PulseAudio does).
	
	uint32_t nFrames;
	double fCheckTick;
	long long nCheckFrame, nLastFrame = 0;

	bool bNoMismatch = true;

	// 2112 is the number of ticks within the test song.
	int nMaxCycles =
		std::max( std::ceil( 2112.0 /
							 static_cast<float>(pPref->m_nBufferSize) *
							getTickSize() * 4.0 ),
				  2112.0 ); 

	// Larger number to account for both small buffer sizes and long
	// samples.
	int nMaxCleaningCycles = 5000;
	int nn = 0;

	// Ensure the sampler is clean.
	while ( getSampler()->isRenderingNotes() ) {
		processAudio( pPref->m_nBufferSize );
		incrementTransportPosition( pPref->m_nBufferSize );
		++nn;
		
		// {//DEBUG
		// 	QString msg = QString( "[song mode] nn: %1, note:" ).arg( nn );
		// 	auto pNoteQueue = getSampler()->getPlayingNotesQueue();
		// 	if ( pNoteQueue.size() > 0 ) {
		// 		auto pNote = pNoteQueue[0];
		// 		if ( pNote != nullptr ) {
		// 			msg.append( pNote->toQString("", true ) );
		// 		} else {
		// 			msg.append( " nullptr" );
		// 		}
		// 		DEBUGLOG( msg );
		// 	}
		// }
		
		if ( nn > nMaxCleaningCycles ) {
			qDebug() << "[testNoteEnqueuing] [song mode] Sampler is in weird state";
			return false;
		}
	}
	locate( 0 );

	nn = 0;

	bool bEndOfSongReached = false;

	auto notesInSong = pSong->getAllNotes();

	std::vector<std::shared_ptr<Note>> notesInSongQueue;
	std::vector<std::shared_ptr<Note>> notesInSamplerQueue;

	while ( getDoubleTick() < m_fSongSizeInTicks ) {

		nFrames = frameDist( randomEngine );

		if ( ! bEndOfSongReached ) {
			if ( updateNoteQueue( nFrames ) == -1 ) {
				bEndOfSongReached = true;
			}
		}

		// Add freshly enqueued notes.
		testMergeQueues( &notesInSongQueue,
						 testCopySongNoteQueue() );

		processAudio( nFrames );

		testMergeQueues( &notesInSamplerQueue,
						 getSampler()->getPlayingNotesQueue() );

		incrementTransportPosition( nFrames );

		++nn;
		if ( nn > nMaxCycles ) {
			qDebug() << QString( "[testNoteEnqueuing] end of the song wasn't reached in time. getFrames(): %1, ticks: %2, getTickSize(): %3, m_fSongSizeInTicks: %4, nMaxCycles: %5" )
				.arg( getFrames() )
				.arg( getDoubleTick(), 0, 'f' )
				.arg( getTickSize(), 0, 'f' )
				.arg( m_fSongSizeInTicks, 0, 'f' )
				.arg( nMaxCycles );
			bNoMismatch = false;
			break;
		}
	}

	if ( notesInSongQueue.size() !=
		 notesInSong.size() ) {
		QString sMsg = QString( "[testNoteEnqueuing] [song mode] Mismatch between notes count in Song [%1] and NoteQueue [%2]. Song:\n" )
			.arg( notesInSong.size() ).arg( notesInSongQueue.size() );
		for ( int ii = 0; ii < notesInSong.size(); ++ii  ) {
			auto note = notesInSong[ ii ];
			sMsg.append( QString( "\t[%1] instr: %2, position: %3, noteStart: %4, velocity: %5\n")
						 .arg( ii )
						 .arg( note->get_instrument()->get_name() )
						 .arg( note->get_position() )
						 .arg( note->getNoteStart() )
						 .arg( note->get_velocity() ) );
		}
		sMsg.append( "NoteQueue:\n" );
		for ( int ii = 0; ii < notesInSongQueue.size(); ++ii  ) {
			auto note = notesInSongQueue[ ii ];
			sMsg.append( QString( "\t[%1] instr: %2, position: %3, noteStart: %4, velocity: %5\n")
						 .arg( ii )
						 .arg( note->get_instrument()->get_name() )
						 .arg( note->get_position() )
						 .arg( note->getNoteStart() )
						 .arg( note->get_velocity() ) );
		}

		qDebug().noquote() << sMsg;
		bNoMismatch = false;
	}

	// We have to relax the test for larger buffer sizes. Else, the
	// notes will be already fully processed in and flush from the
	// Sampler before we had the chance to grab and compare them.
	if ( notesInSamplerQueue.size() !=
		 notesInSong.size() &&
		 pPref->m_nBufferSize < 1024 ) {
		QString sMsg = QString( "[testNoteEnqueuing] [song mode] Mismatch between notes count in Song [%1] and Sampler [%2]. Song:\n" )
			.arg( notesInSong.size() ).arg( notesInSamplerQueue.size() );
		for ( int ii = 0; ii < notesInSong.size(); ++ii  ) {
			auto note = notesInSong[ ii ];
			sMsg.append( QString( "\t[%1] instr: %2, position: %3, noteStart: %4, velocity: %5\n")
						 .arg( ii )
						 .arg( note->get_instrument()->get_name() )
						 .arg( note->get_position() )
						 .arg( note->getNoteStart() )
						 .arg( note->get_velocity() ) );
		}
		sMsg.append( "SamplerQueue:\n" );
		for ( int ii = 0; ii < notesInSamplerQueue.size(); ++ii  ) {
			auto note = notesInSamplerQueue[ ii ];
			sMsg.append( QString( "\t[%1] instr: %2, position: %3, noteStart: %4, velocity: %5\n")
						 .arg( ii )
						 .arg( note->get_instrument()->get_name() )
						 .arg( note->get_position() )
						 .arg( note->getNoteStart() )
						 .arg( note->get_velocity() ) );
		}

		qDebug().noquote() << sMsg;
		bNoMismatch = false;
	}

	setState( AudioEngine::State::Ready );

	unlock();

	if ( ! bNoMismatch ) {
		return bNoMismatch;
	}

	//////////////////////////////////////////////////////////////////
	// Perform the test in pattern mode
	//////////////////////////////////////////////////////////////////
	
	pCoreActionController->activateSongMode( false );
	pHydrogen->setPatternMode( Song::PatternMode::Selected );
	pHydrogen->setSelectedPatternNumber( 4 );

	lock( RIGHT_HERE );

	// For this call the AudioEngine still needs to be in state
	// Playing or Ready.
	reset( false );

	setState( AudioEngine::State::Testing );

	int nLoops = 5;
	
	nMaxCycles = MAX_NOTES * 2 * nLoops;
	nn = 0;

	// Ensure the sampler is clean.
	while ( getSampler()->isRenderingNotes() ) {
		processAudio( pPref->m_nBufferSize );
		incrementTransportPosition( pPref->m_nBufferSize );
		++nn;
		
		// {//DEBUG
		// 	QString msg = QString( "[pattern mode] nn: %1, note:" ).arg( nn );
		// 	auto pNoteQueue = getSampler()->getPlayingNotesQueue();
		// 	if ( pNoteQueue.size() > 0 ) {
		// 		auto pNote = pNoteQueue[0];
		// 		if ( pNote != nullptr ) {
		// 			msg.append( pNote->toQString("", true ) );
		// 		} else {
		// 			msg.append( " nullptr" );
		// 		}
		// 		DEBUGLOG( msg );
		// 	}
		// }
		
		if ( nn > nMaxCleaningCycles ) {
			qDebug() << "[testNoteEnqueuing] [pattern mode] Sampler is in weird state";
			return false;
		}
	}
	locate( 0 );

	auto pPattern = 
		pSong->getPatternList()->get( pHydrogen->getSelectedPatternNumber() );
	if ( pPattern == nullptr ) {
		qDebug() << QString( "[testNoteEnqueuing] null pattern selected [%1]" )
			.arg( pHydrogen->getSelectedPatternNumber() );
		return false;
	}

	std::vector<std::shared_ptr<Note>> notesInPattern;
	for ( int ii = 0; ii < nLoops; ++ii ) {
		FOREACH_NOTE_CST_IT_BEGIN_END( pPattern->get_notes(), it ) {
			if ( it->second != nullptr ) {
				auto note = std::make_shared<Note>( it->second );
				note->set_position( note->get_position() +
									ii * pPattern->get_length() );
				notesInPattern.push_back( note );
			}
		}
	}

	notesInSongQueue.clear();
	notesInSamplerQueue.clear();

	nMaxCycles =
		static_cast<int>(std::max( static_cast<float>(pPattern->get_length()) *
								   static_cast<float>(nLoops) *
								   getTickSize() * 4 /
								   static_cast<float>(pPref->m_nBufferSize),
								   static_cast<float>(MAX_NOTES) *
								   static_cast<float>(nLoops) ));
	nn = 0;

	while ( getDoubleTick() < pPattern->get_length() * nLoops ) {

		nFrames = frameDist( randomEngine );

		updateNoteQueue( nFrames );

		// Add freshly enqueued notes.
		testMergeQueues( &notesInSongQueue,
						 testCopySongNoteQueue() );

		processAudio( nFrames );

		testMergeQueues( &notesInSamplerQueue,
						 getSampler()->getPlayingNotesQueue() );

		incrementTransportPosition( nFrames );

		++nn;
		if ( nn > nMaxCycles ) {
			qDebug() << QString( "[testNoteEnqueuing] end of the pattern wasn't reached in time. getFrames(): %1, ticks: %2, getTickSize(): %3, pattern length: %4, nMaxCycles: %5, nLoops: %6" )
				.arg( getFrames() )
				.arg( getDoubleTick(), 0, 'f' )
				.arg( getTickSize(), 0, 'f' )
				.arg( pPattern->get_length() )
				.arg( nMaxCycles )
				.arg( nLoops );
			bNoMismatch = false;
			break;
		}
	}

	// Transport in pattern mode is always looped. We have to pop the
	// notes added during the second run due to the lookahead.
	int nNoteNumber = notesInSongQueue.size();
	for( int ii = 0; ii < nNoteNumber; ++ii ) {
		auto note = notesInSongQueue[ nNoteNumber - 1 - ii ];
		if ( note != nullptr &&
			 note->get_position() >= pPattern->get_length() * nLoops ) {
			notesInSongQueue.pop_back();
		} else {
			break;
		}
	}

	nNoteNumber = notesInSamplerQueue.size();
	for( int ii = 0; ii < nNoteNumber; ++ii ) {
		auto note = notesInSamplerQueue[ nNoteNumber - 1 - ii ];
		if ( note != nullptr &&
			 note->get_position() >= pPattern->get_length() * nLoops ) {
			notesInSamplerQueue.pop_back();
		} else {
			break;
		}
	}

	if ( notesInSongQueue.size() !=
		 notesInPattern.size() ) {
		QString sMsg = QString( "[testNoteEnqueuing] [pattern mode] Mismatch between notes count in Pattern [%1] and NoteQueue [%2]. Pattern:\n" )
			.arg( notesInPattern.size() ).arg( notesInSongQueue.size() );
		for ( int ii = 0; ii < notesInPattern.size(); ++ii  ) {
			auto note = notesInPattern[ ii ];
			sMsg.append( QString( "\t[%1] instr: %2, position: %3, noteStart: %4, velocity: %5\n")
						 .arg( ii )
						 .arg( note->get_instrument()->get_name() )
						 .arg( note->get_position() )
						 .arg( note->getNoteStart() )
						 .arg( note->get_velocity() ) );
		}
		sMsg.append( "NoteQueue:\n" );
		for ( int ii = 0; ii < notesInSongQueue.size(); ++ii  ) {
			auto note = notesInSongQueue[ ii ];
			sMsg.append( QString( "\t[%1] instr: %2, position: %3, noteStart: %4, velocity: %5\n")
						 .arg( ii )
						 .arg( note->get_instrument()->get_name() )
						 .arg( note->get_position() )
						 .arg( note->getNoteStart() )
						 .arg( note->get_velocity() ) );
		}

		qDebug().noquote() << sMsg;
		bNoMismatch = false;
	}

	// We have to relax the test for larger buffer sizes. Else, the
	// notes will be already fully processed in and flush from the
	// Sampler before we had the chance to grab and compare them.
	if ( notesInSamplerQueue.size() !=
		 notesInPattern.size() &&
		 pPref->m_nBufferSize < 1024 ) {
		QString sMsg = QString( "[testNoteEnqueuing] [pattern mode] Mismatch between notes count in Pattern [%1] and Sampler [%2]. Pattern:\n" )
			.arg( notesInPattern.size() ).arg( notesInSamplerQueue.size() );
		for ( int ii = 0; ii < notesInPattern.size(); ++ii  ) {
			auto note = notesInPattern[ ii ];
			sMsg.append( QString( "\t[%1] instr: %2, position: %3, noteStart: %4, velocity: %5\n")
						 .arg( ii )
						 .arg( note->get_instrument()->get_name() )
						 .arg( note->get_position() )
						 .arg( note->getNoteStart() )
						 .arg( note->get_velocity() ) );
		}
		sMsg.append( "SamplerQueue:\n" );
		for ( int ii = 0; ii < notesInSamplerQueue.size(); ++ii  ) {
			auto note = notesInSamplerQueue[ ii ];
			sMsg.append( QString( "\t[%1] instr: %2, position: %3, noteStart: %4, velocity: %5\n")
						 .arg( ii )
						 .arg( note->get_instrument()->get_name() )
						 .arg( note->get_position() )
						 .arg( note->getNoteStart() )
						 .arg( note->get_velocity() ) );
		}

		qDebug().noquote() << sMsg;
		bNoMismatch = false;
	}

	setState( AudioEngine::State::Ready );

	unlock();

	//////////////////////////////////////////////////////////////////
	// Perform the test in looped pattern mode
	//////////////////////////////////////////////////////////////////

	// In case the transport is looped the first note was lost the
	// first time transport was wrapped to the beginning again. This
	// occurred just in song mode.
	
	pCoreActionController->activateLoopMode( true );
	pCoreActionController->activateSongMode( true );

	lock( RIGHT_HERE );

	// For this call the AudioEngine still needs to be in state
	// Playing or Ready.
	reset( false );

	setState( AudioEngine::State::Testing );

	nLoops = 1;
	nCheckFrame = 0;
	nLastFrame = 0;

	// 2112 is the number of ticks within the test song.
	nMaxCycles =
		std::max( std::ceil( 2112.0 /
							 static_cast<float>(pPref->m_nBufferSize) *
							getTickSize() * 4.0 ),
				  2112.0 ) *
		( nLoops + 1 );
	
	nn = 0;
	// Ensure the sampler is clean.
	while ( getSampler()->isRenderingNotes() ) {
		processAudio( pPref->m_nBufferSize );
		incrementTransportPosition( pPref->m_nBufferSize );
		++nn;
		
		// {//DEBUG
		// 	QString msg = QString( "[song mode] [loop mode] nn: %1, note:" ).arg( nn );
		// 	auto pNoteQueue = getSampler()->getPlayingNotesQueue();
		// 	if ( pNoteQueue.size() > 0 ) {
		// 		auto pNote = pNoteQueue[0];
		// 		if ( pNote != nullptr ) {
		// 			msg.append( pNote->toQString("", true ) );
		// 		} else {
		// 			msg.append( " nullptr" );
		// 		}
		// 		DEBUGLOG( msg );
		// 	}
		// }
		
		if ( nn > nMaxCleaningCycles ) {
			qDebug() << "[testNoteEnqueuing] [loop mode] Sampler is in weird state";
			return false;
		}
	}
	locate( 0 );

	nn = 0;

	bEndOfSongReached = false;

	notesInSong.clear();
	for ( int ii = 0; ii <= nLoops; ++ii ) {
		auto notesVec = pSong->getAllNotes();
		for ( auto nnote : notesVec ) {
			nnote->set_position( nnote->get_position() +
								 ii * m_fSongSizeInTicks );
		}
		notesInSong.insert( notesInSong.end(), notesVec.begin(), notesVec.end() );
	}

	notesInSongQueue.clear();
	notesInSamplerQueue.clear();

	while ( getDoubleTick() < m_fSongSizeInTicks * ( nLoops + 1 ) ) {

		nFrames = frameDist( randomEngine );

		// Turn off loop mode once we entered the last loop cycle.
		if ( ( getDoubleTick() > m_fSongSizeInTicks * nLoops + 100 ) &&
			 pSong->getLoopMode() == Song::LoopMode::Enabled ) {
			INFOLOG( QString( "\n\ndisabling loop mode\n\n" ) );
			pCoreActionController->activateLoopMode( false );
		}

		if ( ! bEndOfSongReached ) {
			if ( updateNoteQueue( nFrames ) == -1 ) {
				bEndOfSongReached = true;
			}
		}

		// Add freshly enqueued notes.
		testMergeQueues( &notesInSongQueue,
						 testCopySongNoteQueue() );

		processAudio( nFrames );

		testMergeQueues( &notesInSamplerQueue,
						 getSampler()->getPlayingNotesQueue() );

		incrementTransportPosition( nFrames );

		++nn;
		if ( nn > nMaxCycles ) {
			qDebug() << QString( "[testNoteEnqueuing] [loop mode] end of the song wasn't reached in time. getFrames(): %1, ticks: %2, getTickSize(): %3, m_fSongSizeInTicks: %4, nMaxCycles: %5" )
				.arg( getFrames() )
				.arg( getDoubleTick(), 0, 'f' )
				.arg( getTickSize(), 0, 'f' )
				.arg( m_fSongSizeInTicks, 0, 'f' )
				.arg( nMaxCycles );
			bNoMismatch = false;
			break;
		}
	}

	if ( notesInSongQueue.size() !=
		 notesInSong.size() ) {
		QString sMsg = QString( "[testNoteEnqueuing] [loop mode] Mismatch between notes count in Song [%1] and NoteQueue [%2]. Song:\n" )
			.arg( notesInSong.size() ).arg( notesInSongQueue.size() );
		for ( int ii = 0; ii < notesInSong.size(); ++ii  ) {
			auto note = notesInSong[ ii ];
			sMsg.append( QString( "\t[%1] instr: %2, position: %3, noteStart: %4, velocity: %5\n")
						 .arg( ii )
						 .arg( note->get_instrument()->get_name() )
						 .arg( note->get_position() )
						 .arg( note->getNoteStart() )
						 .arg( note->get_velocity() ) );
		}
		sMsg.append( "NoteQueue:\n" );
		for ( int ii = 0; ii < notesInSongQueue.size(); ++ii  ) {
			auto note = notesInSongQueue[ ii ];
			sMsg.append( QString( "\t[%1] instr: %2, position: %3, noteStart: %4, velocity: %5\n")
						 .arg( ii )
						 .arg( note->get_instrument()->get_name() )
						 .arg( note->get_position() )
						 .arg( note->getNoteStart() )
						 .arg( note->get_velocity() ) );
		}

		qDebug().noquote() << sMsg;
		bNoMismatch = false;
	}

	// We have to relax the test for larger buffer sizes. Else, the
	// notes will be already fully processed in and flush from the
	// Sampler before we had the chance to grab and compare them.
	if ( notesInSamplerQueue.size() !=
		 notesInSong.size() &&
		 pPref->m_nBufferSize < 1024 ) {
		QString sMsg = QString( "[testNoteEnqueuing] [loop mode] Mismatch between notes count in Song [%1] and Sampler [%2]. Song:\n" )
			.arg( notesInSong.size() ).arg( notesInSamplerQueue.size() );
		for ( int ii = 0; ii < notesInSong.size(); ++ii  ) {
			auto note = notesInSong[ ii ];
			sMsg.append( QString( "\t[%1] instr: %2, position: %3, noteStart: %4, velocity: %5\n")
						 .arg( ii )
						 .arg( note->get_instrument()->get_name() )
						 .arg( note->get_position() )
						 .arg( note->getNoteStart() )
						 .arg( note->get_velocity() ) );
		}
		sMsg.append( "SamplerQueue:\n" );
		for ( int ii = 0; ii < notesInSamplerQueue.size(); ++ii  ) {
			auto note = notesInSamplerQueue[ ii ];
			sMsg.append( QString( "\t[%1] instr: %2, position: %3, noteStart: %4, velocity: %5\n")
						 .arg( ii )
						 .arg( note->get_instrument()->get_name() )
						 .arg( note->get_position() )
						 .arg( note->getNoteStart() )
						 .arg( note->get_velocity() ) );
		}

		qDebug().noquote() << sMsg;
		bNoMismatch = false;
	}
	
	setState( AudioEngine::State::Ready );

	unlock();

	return bNoMismatch;
}

void AudioEngine::testMergeQueues( std::vector<std::shared_ptr<Note>>* noteList, std::vector<std::shared_ptr<Note>> newNotes ) {
	bool bNoteFound;
	for ( const auto& newNote : newNotes ) {
		bNoteFound = false;
		// Check whether the notes is already present.
		for ( const auto& presentNote : *noteList ) {
			if ( newNote != nullptr && presentNote != nullptr ) {
				if ( newNote->match( presentNote.get() ) &&
					 newNote->get_position() ==
					 presentNote->get_position() &&
					 newNote->get_velocity() ==
					 presentNote->get_velocity() ) {
					bNoteFound = true;
				}
			}
		}

		if ( ! bNoteFound ) {
			noteList->push_back( std::make_shared<Note>(newNote.get()) );
		}
	}
}

// Used for the Sampler note queue
void AudioEngine::testMergeQueues( std::vector<std::shared_ptr<Note>>* noteList, std::vector<Note*> newNotes ) {
	bool bNoteFound;
	for ( const auto& newNote : newNotes ) {
		bNoteFound = false;
		// Check whether the notes is already present.
		for ( const auto& presentNote : *noteList ) {
			if ( newNote != nullptr && presentNote != nullptr ) {
				if ( newNote->match( presentNote.get() ) &&
					 newNote->get_position() ==
					 presentNote->get_position() &&
					 newNote->get_velocity() ==
					 presentNote->get_velocity() ) {
					bNoteFound = true;
				}
			}
		}

		if ( ! bNoteFound ) {
			noteList->push_back( std::make_shared<Note>(newNote) );
		}
	}
}

bool AudioEngine::testCheckTransportPosition( const QString& sContext ) const {

	auto pHydrogen = Hydrogen::get_instance();
	auto pSong = pHydrogen->getSong();

	double fCheckTickMismatch;
	long long nCheckFrame = computeFrameFromTick( getDoubleTick(), &fCheckTickMismatch );
	double fCheckTick = computeTickFromFrame( getFrames() );
	
	if ( abs( fCheckTick + fCheckTickMismatch - getDoubleTick() ) > 1e-9 ||
		 abs( fCheckTickMismatch - m_fTickMismatch ) > 1e-9 ||
		 nCheckFrame != getFrames() ) {
		qDebug() << QString( "[testCheckTransportPosition] [%9] [tick or frame mismatch]. getFrames(): %1, nCheckFrame: %2, getDoubleTick(): %3, fCheckTick: %4, m_fTickMismatch: %5, fCheckTickMismatch: %6, getTickSize(): %7, getBpm(): %8, fCheckTick + fCheckTickMismatch - getDoubleTick(): %10, fCheckTickMismatch - m_fTickMismatch: %11, nCheckFrame - getFrames(): %12" )
			.arg( getFrames() )
			.arg( nCheckFrame )
			.arg( getDoubleTick(), 0 , 'f', 9 )
			.arg( fCheckTick, 0 , 'f', 9 )
			.arg( m_fTickMismatch, 0 , 'f', 9 )
			.arg( fCheckTickMismatch, 0 , 'f', 9 )
			.arg( getTickSize(), 0 , 'f' )
			.arg( getBpm(), 0 , 'f' )
			.arg( sContext )
			.arg( fCheckTick + fCheckTickMismatch - getDoubleTick(), 0, 'E' )
			.arg( fCheckTickMismatch - m_fTickMismatch, 0, 'E' )
			.arg( nCheckFrame - getFrames() );

		return false;
	}

	long nCheckPatternStartTick;
	int nCheckColumn = pHydrogen->getColumnForTick( getTick(), pSong->isLoopEnabled(),
													&nCheckPatternStartTick );
	long nTicksSinceSongStart =
		static_cast<long>(std::floor( std::fmod( getDoubleTick(),
												 m_fSongSizeInTicks ) ));
	if ( pHydrogen->getMode() == Song::Mode::Song &&
		 ( nCheckColumn != m_nColumn ||
		   nCheckPatternStartTick != m_nPatternStartTick ||
		   nTicksSinceSongStart - nCheckPatternStartTick != m_nPatternTickPosition ) ) {
		qDebug() << QString( "[testCheckTransportPosition] [%10] [column or pattern tick mismatch]. getTick(): %1, m_nColumn: %2, nCheckColumn: %3, m_nPatternStartTick: %4, nCheckPatternStartTick: %5, m_nPatternTickPosition: %6, nCheckPatternTickPosition: %7, nTicksSinceSongStart: %8, m_fSongSizeInTicks: %9" )
			.arg( getTick() )
			.arg( m_nColumn )
			.arg( nCheckColumn )
			.arg( m_nPatternStartTick )
			.arg( nCheckPatternStartTick )
			.arg( m_nPatternTickPosition )
			.arg( nTicksSinceSongStart - nCheckPatternStartTick )
			.arg( nTicksSinceSongStart )
			.arg( m_fSongSizeInTicks, 0, 'f' )
			.arg( sContext );
		return false;
	}

	return true;
}

bool AudioEngine::testCheckAudioConsistency( const std::vector<std::shared_ptr<Note>> oldNotes,
											 const std::vector<std::shared_ptr<Note>> newNotes, 
											 const QString& sContext,
											 int nPassedFrames, bool bTestAudio,
											 float fPassedTicks ) const {

	bool bNoMismatch = true;
	double fPassedFrames = static_cast<double>(nPassedFrames);
	auto pSong = Hydrogen::get_instance()->getSong();
	
	int nNotesFound = 0;
	for ( const auto& ppNewNote : newNotes ) {
		for ( const auto& ppOldNote : oldNotes ) {
			if ( ppNewNote->match( ppOldNote.get() ) &&
				 ppNewNote->get_humanize_delay() ==
				 ppOldNote->get_humanize_delay() &&
				 ppNewNote->get_velocity() ==
				 ppOldNote->get_velocity() ) {
				++nNotesFound;

				if ( bTestAudio ) {
					// Check for consistency in the Sample position
					// advanced by the Sampler upon rendering.
					for ( int nn = 0; nn < ppNewNote->get_instrument()->get_components()->size(); nn++ ) {
						auto pSelectedLayer = ppOldNote->get_layer_selected( nn );
						
						// The frames passed during the audio
						// processing depends on the sample rate of
						// the driver and sample and has to be
						// adjusted in here. This is equivalent to the
						// question whether Sampler::renderNote() or
						// Sampler::renderNoteResample() was used.
						if ( ppOldNote->getSample( nn )->get_sample_rate() !=
							 Hydrogen::get_instance()->getAudioOutput()->getSampleRate() ||
							 ppOldNote->get_total_pitch() != 0.0 ) {
							// In here we assume the layer pitcyh is zero.
							fPassedFrames = static_cast<double>(nPassedFrames) *
								Note::pitchToFrequency( ppOldNote->get_total_pitch() ) *
								static_cast<float>(ppOldNote->getSample( nn )->get_sample_rate()) /
								static_cast<float>(Hydrogen::get_instance()->getAudioOutput()->getSampleRate());
						}
						
						int nSampleFrames = ( ppNewNote->get_instrument()->get_component( nn )
											  ->get_layer( pSelectedLayer->SelectedLayer )->get_sample()->get_frames() );
						double fExpectedFrames =
							std::min( static_cast<double>(pSelectedLayer->SamplePosition) +
									  fPassedFrames,
									  static_cast<double>(nSampleFrames) );
						if ( std::abs( ppNewNote->get_layer_selected( nn )->SamplePosition -
									   fExpectedFrames ) > 1 ) {
							qDebug().noquote() << QString( "[testCheckAudioConsistency] [%4] glitch in audio render. Diff: %9\nPre: %1\nPost: %2\nwith passed frames: %3, nSampleFrames: %5, fExpectedFrames: %6, sample sampleRate: %7, driver sampleRate: %8\n" )
								.arg( ppOldNote->toQString( "", true ) )
								.arg( ppNewNote->toQString( "", true ) )
								.arg( fPassedFrames, 0, 'f' )
								.arg( sContext )
								.arg( nSampleFrames )
								.arg( fExpectedFrames, 0, 'f' )
								.arg( ppOldNote->getSample( nn )->get_sample_rate() )
								.arg( Hydrogen::get_instance()->getAudioOutput()->getSampleRate() )
								.arg( ppNewNote->get_layer_selected( nn )->SamplePosition -
									  fExpectedFrames, 0, 'g', 30 );
						bNoMismatch = false;
						}
					}
				}
				else { // if ( bTestAudio )
					// Check whether changes in note start position
					// were properly applied in the note queue of the
					// audio engine.
					if ( ppNewNote->get_position() - fPassedTicks !=
						 ppOldNote->get_position() ) {
						qDebug().noquote() << QString( "[testCheckAudioConsistency] [%5] glitch in note queue.\n\tPre: %1\n\tPost: %2\n\tfPassedTicks: %3, diff (new - passed - old): %4" )
							.arg( ppOldNote->toQString( "", true ) )
							.arg( ppNewNote->toQString( "", true ) )
							.arg( fPassedTicks )
							.arg( ppNewNote->get_position() - fPassedTicks -
								  ppOldNote->get_position() )
							.arg( sContext );
						bNoMismatch = false;
					}
				}
			}
		}
	}

	// If one of the note vectors is empty - especially the new notes
	// - we can not test anything. But such things might happen as we
	// try various sample sizes and all notes might be already played
	// back and flushed.
	if ( nNotesFound == 0 &&
		 oldNotes.size() > 0 &&
		 newNotes.size() > 0 ) {
		qDebug() << QString( "[testCheckAudioConsistency] [%1] bad test design. No notes played back." )
			.arg( sContext );
		if ( oldNotes.size() != 0 ) {
			qDebug() << "old notes:";
			for ( auto const& nnote : oldNotes ) {
				qDebug() << nnote->toQString( "    ", true );
			}
		}
		if ( newNotes.size() != 0 ) {
			qDebug() << "new notes:";
			for ( auto const& nnote : newNotes ) {
				qDebug() << nnote->toQString( "    ", true );
			}
		}
		qDebug() << QString( "[testCheckAudioConsistency] curr tick: %1, curr frame: %2, nPassedFrames: %3, fPassedTicks: %4, fTickSize: %5" )
			.arg( getDoubleTick(), 0, 'f' )
			.arg( getFrames() )
			.arg( nPassedFrames )
			.arg( fPassedTicks, 0, 'f' )
			.arg( getTickSize(), 0, 'f' );
		qDebug() << "[testCheckAudioConsistency] notes in song:";
		for ( auto const& nnote : pSong->getAllNotes() ) {
			qDebug() << nnote->toQString( "    ", true );
		}
		
		bNoMismatch = false;
	}

	return bNoMismatch;
}

std::vector<std::shared_ptr<Note>> AudioEngine::testCopySongNoteQueue() {
	std::vector<Note*> rawNotes;
	std::vector<std::shared_ptr<Note>> notes;
	for ( ; ! m_songNoteQueue.empty(); m_songNoteQueue.pop() ) {
		rawNotes.push_back( m_songNoteQueue.top() );
		notes.push_back( std::make_shared<Note>( m_songNoteQueue.top() ) );
	}

	for ( auto nnote : rawNotes ) {
		m_songNoteQueue.push( nnote );
	}

	return notes;
}

bool AudioEngine::testToggleAndCheckConsistency( int nToggleColumn, int nToggleRow, const QString& sContext ) {
	auto pHydrogen = Hydrogen::get_instance();
	auto pCoreActionController = pHydrogen->getCoreActionController();
	auto pSong = pHydrogen->getSong();
	
	unsigned long nBufferSize = pHydrogen->getAudioOutput()->getBufferSize();

	updateNoteQueue( nBufferSize );
	processAudio( nBufferSize );
	incrementTransportPosition( nBufferSize );

	auto prevNotes = testCopySongNoteQueue();

	// Cache some stuff in order to compare it later on.
	long nOldSongSize = pSong->lengthInTicks();
	int nOldColumn = m_nColumn;
	float fPrevTempo = getBpm();
	float fPrevTickSize = getTickSize();
	double fPrevTickStart, fPrevTickEnd;
	long long nPrevLeadLag;

	// We need to reset this variable in order for
	// computeTickInterval() to behave like just after a relocation.
	m_fLastTickIntervalEnd = -1;
	nPrevLeadLag = computeTickInterval( &fPrevTickStart, &fPrevTickEnd, nBufferSize );

	std::vector<std::shared_ptr<Note>> notes1, notes2;
	for ( const auto& ppNote : getSampler()->getPlayingNotesQueue() ) {
		notes1.push_back( std::make_shared<Note>( ppNote ) );
	}

	//////
	// Toggle a grid cell prior to the current transport position
	//////
	
	unlock();
	pCoreActionController->toggleGridCell( nToggleColumn, nToggleRow );
	lock( RIGHT_HERE );

	QString sFirstContext = QString( "[testToggleAndCheckConsistency] %1 : 1. toggling" ).arg( sContext );

	// Check whether there is a change in song size
	long nNewSongSize = pSong->lengthInTicks();
	if ( nNewSongSize == nOldSongSize ) {
		qDebug() << QString( "[%1] no change in song size" )
			.arg( sFirstContext );
		return false;
	}

	// Check whether current frame and tick information are still
	// consistent.
	if ( ! testCheckTransportPosition( sFirstContext ) ) {
		return false;
	}

	// m_songNoteQueue have been updated properly.
	auto afterNotes = testCopySongNoteQueue();

	if ( ! testCheckAudioConsistency( prevNotes, afterNotes,
									  sFirstContext + " 1. audio check",
									  0, false, m_fTickOffset ) ) {
		return false;
	}

	// Column must be consistent. Unless the song length shrunk due to
	// the toggling and the previous column was located beyond the new
	// end (in which case transport will be reset to 0).
	if ( nOldColumn < pSong->getPatternGroupVector()->size() ) {
		// Transport was not reset to 0 - happens in most cases.

		if ( nOldColumn != m_nColumn &&
			 nOldColumn < pSong->getPatternGroupVector()->size() ) {
			qDebug() << QString( "[%3] Column changed old: %1, new: %2" )
				.arg( nOldColumn )
				.arg( m_nColumn )
				.arg( sFirstContext );
			return false;
		}

		// We need to reset this variable in order for
		// computeTickInterval() to behave like just after a relocation.
		m_fLastTickIntervalEnd = -1;
		double fTickEnd, fTickStart;
		const long long nLeadLag = computeTickInterval( &fTickStart, &fTickEnd, nBufferSize );
		if ( std::abs( nLeadLag - nPrevLeadLag ) > 1 ) {
			qDebug() << QString( "[%3] LeadLag should be constant since there should be change in tick size. old: %1, new: %2" )
				.arg( nPrevLeadLag ).arg( nLeadLag ).arg( sFirstContext );
			return false;
		}
		if ( std::abs( fTickStart - m_fTickOffset - fPrevTickStart ) > 4e-3 ) {
			qDebug() << QString( "[%5] Mismatch in the start of the tick interval handled by updateNoteQueue new [%1] != [%2] old+offset, old: %3, offset: %4" )
				.arg( fTickStart, 0, 'f' )
				.arg( fPrevTickStart + m_fTickOffset, 0, 'f' )
				.arg( fPrevTickStart, 0, 'f' )
				.arg( m_fTickOffset, 0, 'f' )
				.arg( sFirstContext );
			return false;
		}
		if ( std::abs( fTickEnd - m_fTickOffset - fPrevTickEnd ) > 4e-3 ) {
			qDebug() << QString( "[%5] Mismatch in the end of the tick interval handled by updateNoteQueue new [%1] != [%2] old+offset, old: %3, offset: %4" )
				.arg( fTickEnd, 0, 'f' )
				.arg( fPrevTickEnd + m_fTickOffset, 0, 'f' )
				.arg( fPrevTickEnd, 0, 'f' )
				.arg( m_fTickOffset, 0, 'f' )
				.arg( sFirstContext );
			return false;
		}
	}
	else if ( m_nColumn != 0 &&
			  nOldColumn >= pSong->getPatternGroupVector()->size() ) {
		qDebug() << QString( "[%4] Column reset failed nOldColumn: %1, m_nColumn (new): %2, pSong->getPatternGroupVector()->size() (new): %3" )
			.arg( nOldColumn )
			.arg( m_nColumn )
			.arg( pSong->getPatternGroupVector()->size() )
			.arg( sFirstContext );
		return false;
	}
	
	// Now we emulate that playback continues without any new notes
	// being added and expect the rendering of the notes currently
	// played back by the Sampler to start off precisely where we
	// stopped before the song size change. New notes might still be
	// added due to the lookahead, so, we just check for the
	// processing of notes we already encountered.
	incrementTransportPosition( nBufferSize );
	processAudio( nBufferSize );
	incrementTransportPosition( nBufferSize );
	processAudio( nBufferSize );

	// Check whether tempo and tick size have not changed.
	if ( fPrevTempo != getBpm() || fPrevTickSize != getTickSize() ) {
		qDebug() << QString( "[%1] tempo and ticksize are affected" )
			.arg( sFirstContext );
		return false;
	}

	for ( const auto& ppNote : getSampler()->getPlayingNotesQueue() ) {
		notes2.push_back( std::make_shared<Note>( ppNote ) );
	}

	if ( ! testCheckAudioConsistency( notes1, notes2,
									  sFirstContext + " 2. audio check",
									  nBufferSize * 2 ) ) {
		return false;
	}

	//////
	// Toggle the same grid cell again
	//////

	QString sSecondContext = QString( "[testToggleAndCheckConsistency] %1 : 2. toggling" ).arg( sContext );
	
	notes1.clear();
	for ( const auto& ppNote : getSampler()->getPlayingNotesQueue() ) {
		notes1.push_back( std::make_shared<Note>( ppNote ) );
	}

	// We deal with a slightly artificial situation regarding
	// m_fLastTickIntervalEnd in here. Usually, in addition to
	// incrementTransportPosition() and	processAudio()
	// updateNoteQueue() would have been called too. This would update
	// m_fLastTickIntervalEnd which is not done in here. This way we
	// emulate a situation that occurs when encountering a change in
	// ticksize (passing a tempo marker or a user interaction with the
	// BPM widget) just before the song size changed.
	double fPrevLastTickIntervalEnd = m_fLastTickIntervalEnd;
	nPrevLeadLag = computeTickInterval( &fPrevTickStart, &fPrevTickEnd, nBufferSize );
	m_fLastTickIntervalEnd = fPrevLastTickIntervalEnd;

	nOldColumn = m_nColumn;
	
	unlock();
	pCoreActionController->toggleGridCell( nToggleColumn, nToggleRow );
	lock( RIGHT_HERE );

	// Check whether there is a change in song size
	nOldSongSize = nNewSongSize;
	nNewSongSize = pSong->lengthInTicks();
	if ( nNewSongSize == nOldSongSize ) {
		qDebug() << QString( "[%1] no change in song size" )
			.arg( sSecondContext );
		return false;
	}

	// Check whether current frame and tick information are still
	// consistent.
	if ( ! testCheckTransportPosition( sSecondContext ) ) {
		return false;
	}

	// Check whether the notes already enqueued into the
	// m_songNoteQueue have been updated properly.
	prevNotes.clear();
	prevNotes = testCopySongNoteQueue();
	if ( ! testCheckAudioConsistency( afterNotes, prevNotes,
									  sSecondContext + " 1. audio check",
									  0, false, m_fTickOffset ) ) {
		return false;
	}

	// Column must be consistent. Unless the song length shrunk due to
	// the toggling and the previous column was located beyond the new
	// end (in which case transport will be reset to 0).
	if ( nOldColumn < pSong->getPatternGroupVector()->size() ) {
		// Transport was not reset to 0 - happens in most cases.

		if ( nOldColumn != m_nColumn &&
			 nOldColumn < pSong->getPatternGroupVector()->size() ) {
			qDebug() << QString( "[%3] Column changed old: %1, new: %2" )
				.arg( nOldColumn )
				.arg( m_nColumn )
				.arg( sSecondContext );
			return false;
		}

		double fTickEnd, fTickStart;
		const long long nLeadLag = computeTickInterval( &fTickStart, &fTickEnd, nBufferSize );
		if ( std::abs( nLeadLag - nPrevLeadLag ) > 1 ) {
			qDebug() << QString( "[%3] LeadLag should be constant since there should be change in tick size. old: %1, new: %2" )
				.arg( nPrevLeadLag ).arg( nLeadLag ).arg( sSecondContext );
			return false;
		}
		if ( std::abs( fTickStart - m_fTickOffset - fPrevTickStart ) > 4e-3 ) {
			qDebug() << QString( "[%5] Mismatch in the start of the tick interval handled by updateNoteQueue new [%1] != [%2] old+offset, old: %3, offset: %4" )
				.arg( fTickStart, 0, 'f' )
				.arg( fPrevTickStart + m_fTickOffset, 0, 'f' )
				.arg( fPrevTickStart, 0, 'f' )
				.arg( m_fTickOffset, 0, 'f' )
				.arg( sSecondContext );
			return false;
		}
		if ( std::abs( fTickEnd - m_fTickOffset - fPrevTickEnd ) > 4e-3 ) {
			qDebug() << QString( "[%5] Mismatch in the end of the tick interval handled by updateNoteQueue new [%1] != [%2] old+offset, old: %3, offset: %4" )
				.arg( fTickEnd, 0, 'f' )
				.arg( fPrevTickEnd + m_fTickOffset, 0, 'f' )
				.arg( fPrevTickEnd, 0, 'f' )
				.arg( m_fTickOffset, 0, 'f' )
				.arg( sSecondContext );
			return false;
		}
	}
	else if ( m_nColumn != 0 &&
			  nOldColumn >= pSong->getPatternGroupVector()->size() ) {
		qDebug() << QString( "[%4] Column reset failed nOldColumn: %1, m_nColumn (new): %2, pSong->getPatternGroupVector()->size() (new): %3" )
			.arg( nOldColumn )
			.arg( m_nColumn )
			.arg( pSong->getPatternGroupVector()->size() )
			.arg( sSecondContext );
		return false;
	}

	// Now we emulate that playback continues without any new notes
	// being added and expect the rendering of the notes currently
	// played back by the Sampler to start off precisely where we
	// stopped before the song size change. New notes might still be
	// added due to the lookahead, so, we just check for the
	// processing of notes we already encountered.
	incrementTransportPosition( nBufferSize );
	processAudio( nBufferSize );
	incrementTransportPosition( nBufferSize );
	processAudio( nBufferSize );

	// Check whether tempo and tick size have not changed.
	if ( fPrevTempo != getBpm() || fPrevTickSize != getTickSize() ) {
		qDebug() << QString( "[%1] tempo and ticksize are affected" )
			.arg( sSecondContext );
		return false;
	}

	notes2.clear();
	for ( const auto& ppNote : getSampler()->getPlayingNotesQueue() ) {
		notes2.push_back( std::make_shared<Note>( ppNote ) );
	}

	if ( ! testCheckAudioConsistency( notes1, notes2,
									  sSecondContext + " 2. audio check",
									  nBufferSize * 2 ) ) {
		return false;
	}

	return true;
}

QString AudioEngine::toQString( const QString& sPrefix, bool bShort ) const {
	QString s = Base::sPrintIndention;
	QString sOutput;
	if ( ! bShort ) {
		sOutput = QString( "%1[AudioEngine]\n" ).arg( sPrefix )
			.append( QString( "%1%2m_nFrames: %3\n" ).arg( sPrefix ).arg( s ).arg( getFrames() ) )
			.append( QString( "%1%2m_fTick: %3\n" ).arg( sPrefix ).arg( s ).arg( getDoubleTick(), 0, 'f' ) )
			.append( QString( "%1%2m_nFrameOffset: %3\n" ).arg( sPrefix ).arg( s ).arg( m_nFrameOffset ) )
			.append( QString( "%1%2m_fTickOffset: %3\n" ).arg( sPrefix ).arg( s ).arg( m_fTickOffset, 0, 'f' ) )
			.append( QString( "%1%2m_fTickSize: %3\n" ).arg( sPrefix ).arg( s ).arg( getTickSize(), 0, 'f' ) )
			.append( QString( "%1%2m_fBpm: %3\n" ).arg( sPrefix ).arg( s ).arg( getBpm(), 0, 'f' ) )
			.append( QString( "%1%2m_fNextBpm: %3\n" ).arg( sPrefix ).arg( s ).arg( m_fNextBpm, 0, 'f' ) )
			.append( QString( "%1%2m_state: %3\n" ).arg( sPrefix ).arg( s ).arg( static_cast<int>(m_state) ) )
			.append( QString( "%1%2m_nextState: %3\n" ).arg( sPrefix ).arg( s ).arg( static_cast<int>(m_nextState) ) )
			.append( QString( "%1%2m_currentTickTime: %3 ms\n" ).arg( sPrefix ).arg( s ).arg( m_currentTickTime.tv_sec * 1000 + m_currentTickTime.tv_usec / 1000) )
			.append( QString( "%1%2m_nPatternStartTick: %3\n" ).arg( sPrefix ).arg( s ).arg( m_nPatternStartTick ) )
			.append( QString( "%1%2m_nPatternTickPosition: %3\n" ).arg( sPrefix ).arg( s ).arg( m_nPatternTickPosition ) )
			.append( QString( "%1%2m_nColumn: %3\n" ).arg( sPrefix ).arg( s ).arg( m_nColumn ) )
			.append( QString( "%1%2m_fSongSizeInTicks: %3\n" ).arg( sPrefix ).arg( s ).arg( m_fSongSizeInTicks, 0, 'f' ) )
			.append( QString( "%1%2m_fTickMismatch: %3\n" ).arg( sPrefix ).arg( s ).arg( m_fTickMismatch, 0, 'f' ) )
			.append( QString( "%1%2m_fLastTickIntervalEnd: %3\n" ).arg( sPrefix ).arg( s ).arg( m_fLastTickIntervalEnd ) )
			.append( QString( "%1%2m_pSampler: \n" ).arg( sPrefix ).arg( s ) )
			.append( QString( "%1%2m_pSynth: \n" ).arg( sPrefix ).arg( s ) )
			.append( QString( "%1%2m_pAudioDriver: \n" ).arg( sPrefix ).arg( s ) )
			.append( QString( "%1%2m_pMidiDriver: \n" ).arg( sPrefix ).arg( s ) )
			.append( QString( "%1%2m_pMidiDriverOut: \n" ).arg( sPrefix ).arg( s ) )
			.append( QString( "%1%2m_pEventQueue: \n" ).arg( sPrefix ).arg( s ) );
#ifdef H2CORE_HAVE_LADSPA
		sOutput.append( QString( "%1%2m_fFXPeak_L: [" ).arg( sPrefix ).arg( s ) );
		for ( auto ii : m_fFXPeak_L ) {
			sOutput.append( QString( " %1" ).arg( ii ) );
		}
		sOutput.append( QString( "]\n%1%2m_fFXPeak_R: [" ).arg( sPrefix ).arg( s ) );
		for ( auto ii : m_fFXPeak_R ) {
			sOutput.append( QString( " %1" ).arg( ii ) );
		}
		sOutput.append( QString( " ]\n" ) );
#endif
		sOutput.append( QString( "%1%2m_fMasterPeak_L: %3\n" ).arg( sPrefix ).arg( s ).arg( m_fMasterPeak_L ) )
			.append( QString( "%1%2m_fMasterPeak_R: %3\n" ).arg( sPrefix ).arg( s ).arg( m_fMasterPeak_R ) )
			.append( QString( "%1%2m_fProcessTime: %3\n" ).arg( sPrefix ).arg( s ).arg( m_fProcessTime ) )
			.append( QString( "%1%2m_fMaxProcessTime: %3\n" ).arg( sPrefix ).arg( s ).arg( m_fMaxProcessTime ) )
			.append( QString( "%1%2m_pNextPatterns: %3\n" ).arg( sPrefix ).arg( s ).arg( m_pNextPatterns->toQString( sPrefix + s ), bShort ) )
			.append( QString( "%1%2m_pPlayingPatterns: %3\n" ).arg( sPrefix ).arg( s ).arg( m_pPlayingPatterns->toQString( sPrefix + s ), bShort ) )
			.append( QString( "%1%2m_nRealtimeFrames: %3\n" ).arg( sPrefix ).arg( s ).arg( m_nRealtimeFrames ) )
			.append( QString( "%1%2m_AudioProcessCallback: \n" ).arg( sPrefix ).arg( s ) )
			.append( QString( "%1%2m_songNoteQueue: length = %3\n" ).arg( sPrefix ).arg( s ).arg( m_songNoteQueue.size() ) );
		sOutput.append( QString( "%1%2m_midiNoteQueue: [\n" ).arg( sPrefix ).arg( s ) );
		for ( const auto& nn : m_midiNoteQueue ) {
			sOutput.append( nn->toQString( sPrefix + s, bShort ) );
		}
		sOutput.append( QString( "]\n%1%2m_pMetronomeInstrument: %3\n" ).arg( sPrefix ).arg( s ).arg( m_pMetronomeInstrument->toQString( sPrefix + s, bShort ) ) )
			.append( QString( "%1%2nMaxTimeHumanize: %3\n" ).arg( sPrefix ).arg( s ).arg( AudioEngine::nMaxTimeHumanize ) );
		
	} else {
		sOutput = QString( "%1[AudioEngine]" ).arg( sPrefix )
			.append( QString( ", m_nFrames: %1" ).arg( getFrames() ) )
			.append( QString( ", m_fTick: %1" ).arg( getDoubleTick(), 0, 'f' ) )
			.append( QString( ", m_nFrameOffset: %1" ).arg( m_nFrameOffset ) )
			.append( QString( ", m_fTickOffset: %1" ).arg( m_fTickOffset, 0, 'f' ) )
			.append( QString( ", m_fTickSize: %1" ).arg( getTickSize(), 0, 'f' ) )
			.append( QString( ", m_fBpm: %1" ).arg( getBpm(), 0, 'f' ) )
			.append( QString( ", m_fNextBpm: %1" ).arg( m_fNextBpm, 0, 'f' ) )
			.append( QString( ", m_state: %1" ).arg( static_cast<int>(m_state) ) )
			.append( QString( ", m_nextState: %1" ).arg( static_cast<int>(m_nextState) ) )
			.append( QString( ", m_currentTickTime: %1 ms" ).arg( m_currentTickTime.tv_sec * 1000 + m_currentTickTime.tv_usec / 1000) )
			.append( QString( ", m_nPatternStartTick: %1" ).arg( m_nPatternStartTick ) )
			.append( QString( ", m_nPatternTickPosition: %1" ).arg( m_nPatternTickPosition ) )
			.append( QString( ", m_nColumn: %1" ).arg( m_nColumn ) )
			.append( QString( ", m_fSongSizeInTicks: %1" ).arg( m_fSongSizeInTicks, 0, 'f' ) )
			.append( QString( ", m_fTickMismatch: %1" ).arg( m_fTickMismatch, 0, 'f' ) )
			.append( QString( ", m_fLastTickIntervalEnd: %1" ).arg( m_fLastTickIntervalEnd ) )
			.append( QString( ", m_pSampler:" ) )
			.append( QString( ", m_pSynth:" ) )
			.append( QString( ", m_pAudioDriver:" ) )
			.append( QString( ", m_pMidiDriver:" ) )
			.append( QString( ", m_pMidiDriverOut:" ) )
			.append( QString( ", m_pEventQueue:" ) );
#ifdef H2CORE_HAVE_LADSPA
		sOutput.append( QString( ", m_fFXPeak_L: [" ) );
		for ( auto ii : m_fFXPeak_L ) {
			sOutput.append( QString( " %1" ).arg( ii ) );
		}
		sOutput.append( QString( "], m_fFXPeak_R: [" ) );
		for ( auto ii : m_fFXPeak_R ) {
			sOutput.append( QString( " %1" ).arg( ii ) );
		}
		sOutput.append( QString( " ]" ) );
#endif
		sOutput.append( QString( ", m_fMasterPeak_L: %1" ).arg( m_fMasterPeak_L ) )
			.append( QString( ", m_fMasterPeak_R: %1" ).arg( m_fMasterPeak_R ) )
			.append( QString( ", m_fProcessTime: %1" ).arg( m_fProcessTime ) )
			.append( QString( ", m_fMaxProcessTime: %1" ).arg( m_fMaxProcessTime ) )
			.append( QString( ", m_pNextPatterns: %1" ).arg( m_pNextPatterns->toQString( sPrefix + s ), bShort ) )
			.append( QString( ", m_pPlayingPatterns: %1" ).arg( m_pPlayingPatterns->toQString( sPrefix + s ), bShort ) )
			.append( QString( ", m_nRealtimeFrames: %1" ).arg( m_nRealtimeFrames ) )
			.append( QString( ", m_AudioProcessCallback:" ) )
			.append( QString( ", m_songNoteQueue: length = %1" ).arg( m_songNoteQueue.size() ) );
		sOutput.append( QString( ", m_midiNoteQueue: [" ) );
		for ( const auto& nn : m_midiNoteQueue ) {
			sOutput.append( nn->toQString( sPrefix + s, bShort ) );
		}
		sOutput.append( QString( "], m_pMetronomeInstrument: id = %1" ).arg( m_pMetronomeInstrument->get_id() ) )
			.append( QString( ", nMaxTimeHumanize: id %1" ).arg( AudioEngine::nMaxTimeHumanize ) );
	}
	
	return sOutput;
}

void AudioEngineLocking::assertAudioEngineLocked() const 
{
#ifndef NDEBUG
		if ( m_bNeedsLock ) {
			H2Core::Hydrogen::get_instance()->getAudioEngine()->assertLocked();
		}
#endif
}

}; // namespace H2Core
