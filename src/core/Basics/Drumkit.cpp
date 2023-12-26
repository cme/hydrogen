/*
 * Hydrogen
 * Copyright(c) 2002-2008 by Alex >Comix< Cominu [comix@users.sourceforge.net]
 * Copyright(c) 2008-2023 The hydrogen development team [hydrogen-devel@lists.sourceforge.net]
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

#include <core/Basics/Drumkit.h>
#include <core/config.h>
#ifdef H2CORE_HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#else
#ifndef WIN32
#include <fcntl.h>
#include <errno.h>
#include <zlib.h>
#include <libtar.h>
#endif
#endif

#include <core/Basics/Sample.h>
#include <core/Basics/DrumkitComponent.h>
#include <core/Basics/DrumkitMap.h>
#include <core/Basics/Instrument.h>
#include <core/Basics/InstrumentList.h>
#include <core/Basics/InstrumentComponent.h>
#include <core/Basics/InstrumentLayer.h>

#include <core/Helpers/Filesystem.h>
#include <core/Helpers/Xml.h>
#include <core/Helpers/Legacy.h>

#include <core/Hydrogen.h>
#include <core/Preferences/Preferences.h>
#include <core/NsmClient.h>
#include <core/SoundLibrary/SoundLibraryDatabase.h>

namespace H2Core
{

Drumkit::Drumkit() : m_bSamplesLoaded( false ),
					 m_pInstruments( nullptr ),
					 m_type( Type::User ),
					 m_sName( "empty" ),
					 m_sAuthor( "undefined author" ),
					 m_sInfo( "No information available." ),
					 m_license( License() ),
					 m_imageLicense( License() )
{
	QDir usrDrumkitPath( Filesystem::usr_drumkits_dir() );
	m_sPath = usrDrumkitPath.filePath( m_sName );
	m_pComponents = std::make_shared<std::vector<std::shared_ptr<DrumkitComponent>>>();
	m_pInstruments = std::make_shared<InstrumentList>();

	m_pDrumkitMap = std::make_shared<DrumkitMap>();
	m_pDrumkitMapFallback = std::make_shared<DrumkitMap>();
}

Drumkit::Drumkit( std::shared_ptr<Drumkit> other ) :
	Object(),
	m_type( other->getType() ),
	m_sPath( other->getPath() ),
	m_sName( other->getName() ),
	m_sAuthor( other->getAuthor() ),
	m_sInfo( other->getInfo() ),
	m_license( other->getLicense() ),
	m_sImage( other->getImage() ),
	m_imageLicense( other->getImageLicense() ),
	m_bSamplesLoaded( other->areSamplesLoaded() )
{
	m_pInstruments = std::make_shared<InstrumentList>( other->getInstruments() );

	m_pComponents = std::make_shared<std::vector<std::shared_ptr<DrumkitComponent>>>();
	for ( const auto& pComponent : *other->getComponents() ) {
		m_pComponents->push_back( std::make_shared<DrumkitComponent>( pComponent ) );
	}

	m_pDrumkitMap = std::make_shared<DrumkitMap>( other->m_pDrumkitMap );
	m_pDrumkitMapFallback = std::make_shared<DrumkitMap>( other->m_pDrumkitMapFallback );
}

Drumkit::~Drumkit()
{
}

std::shared_ptr<Drumkit> Drumkit::getEmptyDrumkit() {

	/*: Name assigned to a fresh Drumkit created via the Main Menu > Drumkit >
	 *  New. */
	const QString sDrumkitName = QT_TRANSLATE_NOOP( "Drumkit", "New Drumkit");
	/*: Name assigned to a DrumkitComponent of a fresh kit created via the Main
	 *  Menu > Drumkit > New. */
	const QString sComponentName = QT_TRANSLATE_NOOP( "Drumkit", "Main");
	/*: Name assigned to an Instrument created either as part of a fresh kit
	 *  created via the Main Menu > Drumkit > New or via the "Add Instrument"
	 *  action. */
	const QString sInstrumentName = QT_TRANSLATE_NOOP( "Drumkit", "New Instrument");

	auto pDrumkit = std::make_shared<Drumkit>();
	auto pInstrList = std::make_shared<InstrumentList>();
	auto pNewInstr = std::make_shared<Instrument>( 1, sInstrumentName );
	pInstrList->add( pNewInstr );
	pDrumkit->setInstruments( pInstrList );
	pDrumkit->setName( sDrumkitName );

	// This has to be done after adding an instrument list. This way it is
	// ensured proper InstrumentComponents are created and assigned as well.
	pDrumkit->addComponent();
	pDrumkit->getComponents()->front()->set_name( sComponentName );

	return pDrumkit;
}

std::shared_ptr<Drumkit> Drumkit::load( const QString& sDrumkitPath, bool bUpgrade, bool bSilent )
{
	if ( ! Filesystem::drumkit_valid( sDrumkitPath ) ) {
		ERRORLOG( QString( "[%1] is not valid drumkit folder" ).arg( sDrumkitPath ) );
		return nullptr;
	}

	QString sDrumkitFile = Filesystem::drumkit_file( sDrumkitPath );
	
	bool bReadingSuccessful = true;
	
	XMLDoc doc;
	if ( !doc.read( sDrumkitFile, Filesystem::drumkit_xsd_path(), true ) ) {
		// Drumkit does not comply with the XSD schema
		// definition. It's probably an old one. load_from() will try
		// to handle it regardlessly but we should upgrade it in order
		// to avoid this in future loads.
		doc.read( sDrumkitFile, nullptr, bSilent );
		
		bReadingSuccessful = false;
	}
	
	XMLNode root = doc.firstChildElement( "drumkit_info" );
	if ( root.isNull() ) {
		ERRORLOG( "drumkit_info node not found" );
		return nullptr;
	}

	auto pDrumkit =
		Drumkit::loadFrom( &root, sDrumkitFile.left( sDrumkitFile.lastIndexOf( "/" ) ),
						   "", false, bSilent );
	
	if ( pDrumkit == nullptr ) {
		ERRORLOG( QString( "Unable to load drumkit [%1]" ).arg( sDrumkitFile ) );
		return nullptr;
	}

	pDrumkit->setType( DetermineType( pDrumkit->getPath() ) );

	// Load drumkit maps corresponding to this kit.
	//
	// Order of precedeence:
	// 1. USER_DATA_DIR/drumkit_maps/KIT_NAME.h2map
	// 2. *.h2map file in drumkit folder itself
	// 3. SYS_DATA_DIR/drumkit_maps/KIT_NAME.h2map
	//
	// If both 1. and another map file is present, 1. will be assigned to m_pDrumkitMap
	// and the other one to m_pDrumkitMapFallback. In case all there variants are
	// present, only 2. is assigned to m_pDrumkitMapFallback.
	const QString sUserMapFile =
		Filesystem::getDrumkitMapFromDir( pDrumkit->getExportName(), true );

	QString sMapFile = Filesystem::getDrumkitMapFromKit( sDrumkitPath );
	if ( sMapFile.isEmpty() ){
		sMapFile = Filesystem::getDrumkitMapFromDir( pDrumkit->getExportName(), false );
	}

	if ( ! sUserMapFile.isEmpty() ) {
		pDrumkit->m_pDrumkitMap = DrumkitMap::load( sUserMapFile );

		if ( ! sMapFile.isEmpty() ) {
			pDrumkit->m_pDrumkitMapFallback = DrumkitMap::load( sMapFile );
		}
	}
	else if ( ! sMapFile.isEmpty() ) {
		pDrumkit->m_pDrumkitMap = DrumkitMap::load( sMapFile );
	}

	if ( ! bReadingSuccessful && bUpgrade ) {
		pDrumkit->upgrade( bSilent );
	}

	return pDrumkit;
}

std::shared_ptr<Drumkit> Drumkit::loadFrom( XMLNode* node,
											const QString& sDrumkitPath,
											const QString& sSongPath,
											bool bSongKit,
											bool bSilent )
{
	QString sDrumkitName = node->read_string( "name", "", false, false, bSilent );
	if ( sDrumkitName.isEmpty() ) {
		ERRORLOG( "Drumkit has no name, abort" );
		return nullptr;
	}
	
	std::shared_ptr<Drumkit> pDrumkit = std::make_shared<Drumkit>();

	pDrumkit->m_sPath = sDrumkitPath;
	pDrumkit->m_sName = sDrumkitName;
	pDrumkit->m_sAuthor = node->read_string( "author", "undefined author",
											true, true, true );
	pDrumkit->m_sInfo = node->read_string( "info", "No information available.",
										  true, true, bSilent  );

	License license( node->read_string( "license", "undefined license",
										true, true, bSilent  ),
					 pDrumkit->m_sAuthor );
	pDrumkit->setLicense( license );

	// As of 2022 we have no drumkits featuring an image in
	// stock. Thus, verbosity of this one will be turned of in order
	// to make to log more concise.
	pDrumkit->setImage( node->read_string( "image", "",
											true, true, true ) );
	License imageLicense( node->read_string( "imageLicense", "undefined license",
											 true, true, true  ),
						  pDrumkit->m_sAuthor );
	pDrumkit->setImageLicense( imageLicense );

	XMLNode componentListNode = node->firstChildElement( "componentList" );
	if ( ! componentListNode.isNull() ) {
		XMLNode componentNode = componentListNode.firstChildElement( "drumkitComponent" );
		while ( ! componentNode.isNull()  ) {
			auto pDrumkitComponent = DrumkitComponent::load_from( &componentNode );
			if ( pDrumkitComponent != nullptr ) {
				pDrumkit->getComponents()->push_back(pDrumkitComponent);
			}

			componentNode = componentNode.nextSiblingElement( "drumkitComponent" );
		}
	} else {
		WARNINGLOG( "componentList node not found" );
		auto pDrumkitComponent = std::make_shared<DrumkitComponent>( 0, "Main" );
		pDrumkit->getComponents()->push_back(pDrumkitComponent);
	}

	auto pInstrumentList = InstrumentList::load_from(
		node, sDrumkitPath, sDrumkitName, sSongPath, license, bSongKit, false );
	// Required to assure backward compatibility.
	if ( pInstrumentList == nullptr ) {
		WARNINGLOG( "instrument list could not be loaded. Using empty one." );
		pInstrumentList = std::make_shared<InstrumentList>();
	}
		
	pDrumkit->setInstruments( pInstrumentList );

	if ( ! bSongKit ) {
		// Instead of making the *::load_from() functions more complex by
		// passing the license down to each sample, we will make the
		// drumkit assign its license to each sample in here.
		pDrumkit->propagateLicense();
	}

	return pDrumkit;

}

void Drumkit::loadSamples( float fBpm )
{
	INFOLOG( QString( "Loading drumkit %1 instrument samples" ).arg( m_sName ) );
	if( !m_bSamplesLoaded ) {
		m_pInstruments->load_samples( fBpm );
		m_bSamplesLoaded = true;
	}
}

void Drumkit::upgrade( bool bSilent ) {
	if ( !bSilent ) {
		INFOLOG( QString( "Upgrading drumkit [%1] in [%2]" )
				 .arg( m_sName ).arg( m_sPath ) );
	}

	QString sBackupFile = Filesystem::drumkit_backup_path(
		Filesystem::drumkit_file( m_sPath ) );
	Filesystem::file_copy( Filesystem::drumkit_file( m_sPath ),
						   sBackupFile,
						   false, // do not overwrite existing files
						   bSilent );

	save( "", -1, true, bSilent);
}

void Drumkit::unloadSamples() {
        INFOLOG( QString( "Unloading drumkit %1 instrument samples" ).arg( m_sName ) );
	if( m_bSamplesLoaded ) {
		m_pInstruments->unload_samples();
		m_bSamplesLoaded = false;
	}
}

QString Drumkit::getFolderName() const {
	return Filesystem::validateFilePath( m_sName );
}

QString Drumkit::getExportName( const QString& sComponentName, bool bRecentVersion ) const {
	QString sExportName = getFolderName();
	if ( ! sComponentName.isEmpty() ) {
		sExportName.append( "_" +
							Filesystem::validateFilePath( sComponentName ) );
		if ( ! bRecentVersion ) {
			sExportName.append( "_legacy" );
		}
	}
	
	return sExportName;
}

bool Drumkit::save( const QString& sDrumkitPath, int nComponentID, bool bRecentVersion, bool bSilent )
{
	QString sDrumkitFolder( sDrumkitPath );
	if ( sDrumkitPath.isEmpty() ) {
		sDrumkitFolder = m_sPath;
	}
	else {
		// We expect the path to a folder in sDrumkitPath. But in case
		// the user or developer provided the path to the drumkit.xml
		// file within this folder we don't play dumb as such things
		// happen and are plausible when just looking at the
		// function's signature
		QFileInfo fi( sDrumkitPath );
		if ( fi.isFile() && fi.fileName() == Filesystem::drumkit_xml() ) {
			WARNINGLOG( QString( "Please provide the path to the drumkit folder instead to the drumkit.xml file within: [%1]" )
					 .arg( sDrumkitPath ) );
			sDrumkitFolder = fi.dir().absolutePath();
		}
	}
	
	if ( ! Filesystem::dir_exists( sDrumkitFolder, true ) &&
		 ! Filesystem::mkdir( sDrumkitFolder ) ) {
		ERRORLOG( QString( "Unable to export drumkit [%1] to [%2]. Could not create drumkit folder." )
			 .arg( m_sName ).arg( sDrumkitFolder ) );
		return false;
	}

	if ( Filesystem::dir_exists( sDrumkitFolder, bSilent ) &&
		 ! Filesystem::dir_writable( sDrumkitFolder, bSilent ) ) {
		ERRORLOG( QString( "Unable to export drumkit [%1] to [%2]. Drumkit folder not writable." )
			 .arg( m_sName ).arg( sDrumkitFolder ) );
		return false;
	}

	if ( ! bSilent ) {
		INFOLOG( QString( "Saving drumkit [%1] into [%2]" )
				 .arg( m_sName ).arg( sDrumkitFolder ) );
	}

	// Save external files
	if ( ! saveSamples( sDrumkitFolder, bSilent ) ) {
		ERRORLOG( QString( "Unable to save samples of drumkit [%1] to [%2]. Abort." )
				  .arg( m_sName ).arg( sDrumkitFolder ) );
		return false;
	}
	
	if ( ! saveImage( sDrumkitFolder, bSilent ) ) {
		ERRORLOG( QString( "Unable to save image of drumkit [%1] to [%2]. Abort." )
				  .arg( m_sName ).arg( sDrumkitFolder ) );
		return false;
	}

	// Save drumkit map (primary one)
		const QString sDrumkitMapPath = QString( "%1/%2%3" )
			.arg( Filesystem::usr_drumkit_maps_dir() )
			.arg( getExportName() )
			.arg( Filesystem::drumkit_map_ext );
        if ( ! m_pDrumkitMap->save( sDrumkitMapPath ) ) {
			ERRORLOG( QString( "Unable to save drumkit map to [%1]" )
				  .arg( sDrumkitMapPath ) );
		return false;
	}

	// Ensure all instruments and associated samples will hold the
	// same license as the overall drumkit and are associated to
	// it. (Not important for saving itself but for consistency and
	// using the drumkit later on).
	propagateLicense();

	// Save drumkit.xml
	XMLDoc doc;
	XMLNode root = doc.set_root( "drumkit_info", "drumkit" );
	
	// In order to comply with the GPL license we have to add a
	// license notice to the file.
	if ( m_license.getType() == License::GPL ) {
		root.appendChild( doc.createComment( License::getGPLLicenseNotice( m_sAuthor ) ) );
	}
	
	saveTo( &root, nComponentID, bRecentVersion, false, bSilent );
	return doc.write( Filesystem::drumkit_file( sDrumkitFolder ) );
}

void Drumkit::saveTo( XMLNode* node,
					  int component_id,
					  bool bRecentVersion,
					  bool bSongKit,
					  bool bSilent ) const
{
	node->write_string( "name", m_sName );
	node->write_string( "author", m_sAuthor );
	node->write_string( "info", m_sInfo );
	node->write_string( "license", m_license.getLicenseString() );

	QString sImage;
	if ( bSongKit ) {
		sImage = m_sImage;
	}
	else {
		// Other routines take care of copying the image into the (top level of
		// the) selected drumkit folder. We can thus just store the file name of
		// the image.
		sImage = QFileInfo( Filesystem::removeUniquePrefix( m_sImage ) )
			.fileName();
	}
	node->write_string( "image", sImage );
	node->write_string( "imageLicense", m_imageLicense.getLicenseString() );

	// Only drumkits used for Hydrogen v0.9.7 or higher are allowed to
	// have components. If the user decides to export the kit to
	// legacy version, the components will be omitted and Instrument
	// layers corresponding to component_id will be exported.
	if ( bRecentVersion ) {
		XMLNode components_node = node->createNode( "componentList" );
		if ( component_id == -1 && m_pComponents->size() > 0 ) {
			for ( const auto& pComponent : *m_pComponents ){
				pComponent->save_to( &components_node );
			}
		}
		else {
			bool bComponentFound = false;

			if ( component_id != -1 ) {
				for ( const auto& pComponent : *m_pComponents ){
					if ( pComponent != nullptr &&
						 pComponent->get_id() == component_id ) {
						bComponentFound = true;
						pComponent->save_to( &components_node );
					}
				}
			} else {
				WARNINGLOG( "Drumkit has no components. Storing an empty one as fallback." );
			}

			if ( ! bComponentFound ) {
				if ( component_id != -1 ) {
					ERRORLOG( QString( "Unable to retrieve DrumkitComponent [%1]. Storing an empty one as fallback." )
							  .arg( component_id ) );
				}
				auto pDrumkitComponent = std::make_shared<DrumkitComponent>( 0, "Main" );
				pDrumkitComponent->save_to( &components_node );
			}
		}
	} else {
		// Legacy export
		if ( component_id == -1 ) {
			ERRORLOG( "Exporting the full drumkit with all components is allowed when targeting the legacy versions >= 0.9.6" );
			return;
		}
	}

	if ( m_pInstruments != nullptr && m_pInstruments->size() > 0 ) {
		m_pInstruments->save_to( node, component_id, bRecentVersion,
								 bSongKit );
	} else {
		WARNINGLOG( "Drumkit has no instruments. Storing an InstrumentList with a single empty Instrument as fallback." );
		auto pInstrumentList = std::make_shared<InstrumentList>();
		auto pInstrument = std::make_shared<Instrument>();
		pInstrumentList->insert( 0, pInstrument );
		pInstrumentList->save_to( node, component_id, bRecentVersion,
								  bSongKit );
	}
}

bool Drumkit::saveSamples( const QString& sDrumkitFolder, bool bSilent ) const
{
	if ( ! bSilent ) {
		INFOLOG( QString( "Saving drumkit [%1] samples into [%2]" )
				 .arg( m_sName ).arg( sDrumkitFolder ) );
	}

	auto pInstrList = getInstruments();
	for ( int i = 0; i < pInstrList->size(); i++ ) {
		auto pInstrument = ( *pInstrList )[i];
		for ( const auto& pComponent : *pInstrument->get_components() ) {

			for ( int n = 0; n < InstrumentComponent::getMaxLayers(); n++ ) {
				auto pLayer = pComponent->get_layer( n );
				if ( pLayer != nullptr && pLayer->get_sample() != nullptr ) {
					QString src = pLayer->get_sample()->get_filepath();
					QString dst = sDrumkitFolder + "/" + pLayer->get_sample()->get_filename();

					if ( src != dst ) {
						QString original_dst = dst;

						// If the destination path does not have an extension and there is a dot in the path, hell will break loose. QFileInfo maybe?
						int insertPosition = original_dst.length();
						if ( original_dst.lastIndexOf(".") > 0 ) {
							insertPosition = original_dst.lastIndexOf(".");
						}

						pLayer->get_sample()->set_filename( dst );

						if( ! Filesystem::file_copy( src, dst, bSilent ) ) {
							return false;
						}
					}
				}
			}
		}
	}

	return true;
}

bool Drumkit::saveImage( const QString& sDrumkitDir, bool bSilent ) const
{
	if ( m_sImage.isEmpty() ) {
		// No image
		return true;
	}

	// In case we deal with a song kit the associated image was stored in
	// Hydrogen's cache folder (as saving it next to the .h2song file would be
	// not practical and error prone). In order to preserve the original
	// filename, a random prefix was introduced. This has to be stripped first.
	QString sTargetImagePath( getAbsoluteImagePath() );
	QString sTargetImageName( getAbsoluteImagePath() );
	if ( m_type == Type::Song ) {
		sTargetImageName = Filesystem::removeUniquePrefix( sTargetImagePath );
	}

	if ( sTargetImagePath.contains( sDrumkitDir ) ) {
		// Image is already present in target folder.
		return true;
	}

	QFileInfo info( sTargetImageName );
	const QString sDestination =
		QDir( sDrumkitDir ).absoluteFilePath( info.fileName() );

	if ( Filesystem::file_exists( sTargetImagePath, bSilent ) ) {
		if ( ! Filesystem::file_copy( sTargetImagePath, sDestination, bSilent ) ) {
			ERRORLOG( QString( "Error copying image [%1] to [%2]")
					  .arg( sTargetImagePath ).arg( sDestination ) );
			return false;
		}
	}
	return true;
}

QString Drumkit::getAbsoluteImagePath() const {
	// No image set.
	if ( m_sImage.isEmpty() ) {
		return std::move( QString() );
	}

	QFileInfo info( m_sImage );
	if ( info.isRelative() ) {
		// Image was stored as plain filename and is located in drumkit folder.
		return std::move( QDir( m_sPath ).absoluteFilePath( m_sImage ) );
	}
	else {
		return m_sImage;
	}
}

void Drumkit::setInstruments( std::shared_ptr<InstrumentList> pInstruments )
{
	m_pInstruments = pInstruments;
	m_bSamplesLoaded = pInstruments->isAnyInstrumentSampleLoaded();
}


void Drumkit::removeInstrument( int nInstrumentNumber ) {
	auto pInstrument = m_pInstruments->get( nInstrumentNumber );
	if ( pInstrument == nullptr ) {
		ERRORLOG( QString( "Unable to retrieve instrument [%1]" )
				  .arg( nInstrumentNumber ) );
		return;
	}

	m_pInstruments->del( nInstrumentNumber );

	// Check which components of this instrument do contain samples
	std::vector componentsWithSamplesIds = std::vector<int>();
	for ( const auto& ppComponent : *pInstrument->get_components() ) {
		if ( ppComponent != nullptr ) {
			for ( const auto& ppLayer : ppComponent->get_layers() ) {
				if ( ppLayer != nullptr && ppLayer->get_sample() != nullptr ) {
					componentsWithSamplesIds.push_back(
						ppComponent->get_drumkit_componentID() );
					break;
				}
			}
		}
	}

	// Check whether there are other instruments holding samples in those
	// components as well. If not, delete the component.
	for ( const auto nnComponentId : componentsWithSamplesIds ) {
		bool bOtherSamplesFound = false;
		for ( const auto& ppInstrument : *m_pInstruments ) {
			if ( ppInstrument != nullptr &&
				 ppInstrument->get_id() != nInstrumentNumber ) {
				for ( const auto& ppComponent : *ppInstrument->get_components() ) {
					if ( ppComponent != nullptr ) {
						for ( const auto& ppLayer : ppComponent->get_layers() ) {
							if ( ppLayer != nullptr && ppLayer->get_sample() != nullptr ) {
								bOtherSamplesFound = true;
								break;
							}
						}
					}

					if ( bOtherSamplesFound ) {
						break;
					}
				}
			}

			if ( bOtherSamplesFound ) {
				break;
			}
		}

		if ( ! bOtherSamplesFound ) {
			INFOLOG( QString( "No other samples found for component [%1]. Removing it." )
					 .arg( nnComponentId ) );
			removeComponent( nnComponentId );
		}
	}
}

void Drumkit::addInstrument() {
	/*: Name assigned to an Instrument created either as part of a fresh kit
	 *  created via the Main Menu > Drumkit > New or via the "Add Instrument"
	 *  action. */
	const QString sInstrumentName = QT_TRANSLATE_NOOP( "Drumkit", "New Instrument");

	auto pNewInstrument = std::make_shared<Instrument>();
	pNewInstrument->set_name( sInstrumentName );

	// The new instrument is manually added to a floating song kit. It must not
	// have a drumkit path or drumkit name set. All contained samples have to be
	// referenced by absolute paths.
	if ( m_type != Type::Song ) {
		pNewInstrument->set_drumkit_name( m_sName );
		pNewInstrument->set_drumkit_path( m_sPath );
	}

	addInstrument( pNewInstrument );
}

void Drumkit::addInstrument( std::shared_ptr<Instrument> pInstrument ) {
	if ( pInstrument == nullptr ) {
		ERRORLOG( "invalid instrument" );
		return;
	}

	// In case a new instrument was added manually, there is no associated
	// drumkit to load samples from.
	std::shared_ptr<Drumkit> pInstrumentKit = nullptr;
	if ( ! pInstrument->get_drumkit_path().isEmpty() ) {
		pInstrumentKit = Hydrogen::get_instance()->getSoundLibraryDatabase()
			->getDrumkit( pInstrument->get_drumkit_path() );
		if ( pInstrumentKit == nullptr ) {
			ERRORLOG( QString( "Unable to retrieve kit [%1] associated with instrument." )
					  .arg( pInstrument->get_drumkit_path() ) );
			return;
		}
	}

	// Ensure instrument components are contained in the Drumkit (compared by
	// name). If not, add them. But due to the original design of the components
	// we have to compare the DrumkitComponents (not the more light-weighted
	// InstrumentComponents). If for some reason we fail the retrieve the
	// drumkit corresponding to the instrument, we can not do the component
	// mapping. But this should not happen.
	for ( const auto& ppInstrumentComponent : *pInstrument->get_components() ) {
		if ( ppInstrumentComponent == nullptr ) {
			continue;
		}

		// In case the instrument has no samples in this component, we skip it.
		bool bSampleFound = false;
		for ( const auto& ppLayer : ppInstrumentComponent->get_layers() ) {
			if ( ppLayer != nullptr && ppLayer->get_sample() != nullptr ) {
				bSampleFound = true;
				break;
			}
		}
		if ( ! bSampleFound ) {
			continue;
		}

		if ( pInstrumentKit == nullptr ) {
			ERRORLOG( "An instrument added to a kit must have either both components and an associated drumkit path or neither of them." );
			return;
		}

		auto ppComponent = pInstrumentKit->getComponent(
			ppInstrumentComponent->get_drumkit_componentID() );

		int nOldID = ppComponent->get_id();

		int nNewId = -1;
		for ( const auto& ppThisKitsComponent : *m_pComponents ) {
			if ( ppThisKitsComponent != nullptr &&
				 ppThisKitsComponent->get_name().compare(
					 ppComponent->get_name() ) == 0 ) {
				nNewId = ppThisKitsComponent->get_id();
			}
		}

		if ( nNewId == -1 ) {
			// No matching component in this drumkit found.
			//
			// Get an ID not used as drumkit component ID by the drumkit
			// currently loaded.
			nNewId = findUnusedComponentId();

			auto pNewComponent = std::make_shared<DrumkitComponent>( ppComponent );
			pNewComponent->set_id( nNewId );

			addComponent( pNewComponent );
		}
		else {
			// Component is already present. We just have to rewire it.
			ppInstrumentComponent->set_drumkit_componentID( nNewId );
		}
	}

	// Add components of this drumkit not already present in the instrument.
	for ( const auto& ppThisKitsComponent : *m_pComponents ) {
		if ( ppThisKitsComponent != nullptr ) {
			bool bIsPresent = false;
			for ( const auto& ppInstrumentCompnent : *pInstrument->get_components() ) {
				if ( ppInstrumentCompnent != nullptr &&
					 ppInstrumentCompnent->get_drumkit_componentID() ==
					 ppThisKitsComponent->get_id() ) {
					bIsPresent = true;
					break;
				}
			}

			if ( ! bIsPresent ){
				auto pNewInstrCompo = std::make_shared<InstrumentComponent>(
					ppThisKitsComponent->get_id() );
				pInstrument->get_components()->push_back( pNewInstrCompo );
			}
		}
	}

	// create a new valid ID for this instrument
	int nNewId = m_pInstruments->size();
	for ( int ii = 0; ii < m_pInstruments->size(); ++ii ) {
		bool bIsPresent = false;
		for ( const auto& ppInstrument : *m_pInstruments ) {
			if ( ppInstrument != nullptr &&
				 ppInstrument->get_id() == ii ) {
				bIsPresent = true;
				break;
			}
		}

		if ( ! bIsPresent ) {
			nNewId = ii;
			break;
		}
	}

	pInstrument->set_id( nNewId );

	m_pInstruments->add( pInstrument );
}

void Drumkit::removeComponent( int nId ) {
	for ( int ii = 0; ii < m_pComponents->size(); ++ii ) {
		auto ppComponent = m_pComponents->at( ii );
		if ( ppComponent != nullptr && ppComponent->get_id() == nId ) {
			m_pComponents->erase( m_pComponents->begin() + ii );
			break;
		}
	}

	for ( const auto& ppInstrument : *m_pInstruments ) {
		if ( ppInstrument != nullptr ) {
			auto pComponents = ppInstrument->get_components();
			for ( int ii = 0; ii < pComponents->size(); ++ii ) {
				auto ppComponent = pComponents->at( ii );
				if ( ppComponent != nullptr &&
					 ppComponent->get_drumkit_componentID() == nId ) {
					pComponents->erase( pComponents->begin() + ii );
					break;
				}
			}
		}
	}
}

int Drumkit::findUnusedComponentId() const {
	int nNewId = m_pComponents->size();
	for ( int ii = 0; ii < m_pComponents->size(); ++ii ) {
		bool bIsPresent = false;
		for ( const auto& ppComp : *m_pComponents ) {
			if ( ppComp != nullptr && ppComp->get_id() == ii ) {
				bIsPresent = true;
				break;
			}
		}

		if ( ! bIsPresent ){
			nNewId = ii;
			break;
		}
	}

	return nNewId;
}

std::shared_ptr<DrumkitComponent> Drumkit::addComponent() {
	auto pNewComponent = std::make_shared<DrumkitComponent>();
	pNewComponent->set_id( findUnusedComponentId() );

	addComponent( pNewComponent );

	return pNewComponent;
}

void Drumkit::addComponent( std::shared_ptr<DrumkitComponent> pComponent ) {
	// Sanity check
	if ( pComponent == nullptr ) {
		ERRORLOG( "Invalid component" );
		return;
	}

	for ( const auto& ppComponent : *m_pComponents ) {
		if ( ppComponent == pComponent ) {
			ERRORLOG( "Component is already present" );
			return;
		}
	}

	m_pComponents->push_back( pComponent );

	for ( auto& ppInstrument : *m_pInstruments ) {
		ppInstrument->get_components()->push_back(
			std::make_shared<InstrumentComponent>(pComponent->get_id()) );
	}
}

void Drumkit::setComponents( std::shared_ptr<std::vector<std::shared_ptr<DrumkitComponent>>> components )
{
	m_pComponents = components;
}

void Drumkit::propagateLicense(){

	for ( const auto& ppInstrument : *m_pInstruments ) {
		if ( ppInstrument != nullptr ) {

			ppInstrument->set_drumkit_path( m_sPath );
			ppInstrument->set_drumkit_name( m_sName );
			for ( const auto& ppInstrumentComponent : *ppInstrument->get_components() ) {
				if ( ppInstrumentComponent != nullptr ) {
					for ( const auto& ppInstrumentLayer : *ppInstrumentComponent ) {
						if ( ppInstrumentLayer != nullptr ) {
							auto pSample = ppInstrumentLayer->get_sample();
							if ( pSample != nullptr ) {
								pSample->setLicense( getLicense() );
							}
						}
					}
				}
			}
		}
	}
}

std::vector<std::shared_ptr<InstrumentList::Content>> Drumkit::summarizeContent() const {
	return m_pInstruments->summarizeContent( m_pComponents );
}

bool Drumkit::install( const QString& sSourcePath,
					   const QString& sTargetPath,
					   QString* pInstalledPath,
					   bool bSilent )
{
	if ( sTargetPath.isEmpty() ) {
		if ( ! bSilent ) {
			_INFOLOG( QString( "Install drumkit [%1]" ).arg( sSourcePath ) );
		}
		
	} else {
		if ( ! Filesystem::path_usable( sTargetPath, true, false ) ) {
			return false;
		}
		
		if ( ! bSilent ) {		
			_INFOLOG( QString( "Extract drumkit from [%1] to [%2]" )
					  .arg( sSourcePath ).arg( sTargetPath ) );
		}
	}
	
#ifdef H2CORE_HAVE_LIBARCHIVE
	int r;
	struct archive* arch;
	struct archive_entry* entry;

	arch = archive_read_new();

#if ARCHIVE_VERSION_NUMBER < 3000000
	archive_read_support_compression_all( arch );
#else
	archive_read_support_filter_all( arch );
#endif

	archive_read_support_format_all( arch );

#if ARCHIVE_VERSION_NUMBER < 3000000
	if ( archive_read_open_file( arch, sSourcePath.toLocal8Bit(), 10240 ) ) {
#else
	if ( archive_read_open_filename( arch, sSourcePath.toLocal8Bit(), 10240 ) ) {
#endif
		_ERRORLOG( QString( "archive_read_open_file() [%1] %2" )
				   .arg( archive_errno( arch ) )
				   .arg( archive_error_string( arch ) ) );
		archive_read_close( arch );

#if ARCHIVE_VERSION_NUMBER < 3000000
		archive_read_finish( arch );
#else
		archive_read_free( arch );
#endif

		return false;
	}
	bool ret = true;

	QString dk_dir;
	if ( ! sTargetPath.isEmpty() ) {
		dk_dir = sTargetPath + "/";
	} else {
		dk_dir = Filesystem::usr_drumkits_dir() + "/";
	}

	// Keep track of where the artifacts where extracted to
	QString sExtractedDir = "";
		
	while ( ( r = archive_read_next_header( arch, &entry ) ) != ARCHIVE_EOF ) {
		if ( r != ARCHIVE_OK ) {
			_ERRORLOG( QString( "archive_read_next_header() [%1] %2" )
					   .arg( archive_errno( arch ) )
					   .arg( archive_error_string( arch ) ) );
			ret = false;
			break;
		}
		QString np = dk_dir + archive_entry_pathname( entry );

		if ( np.contains( Filesystem::drumkit_xml() ) ) {
			QFileInfo npInfo( np );
			sExtractedDir = npInfo.absoluteDir().absolutePath();
		}

		QByteArray newpath = np.toLocal8Bit();

		archive_entry_set_pathname( entry, newpath.data() );
		r = archive_read_extract( arch, entry, 0 );
		if ( r == ARCHIVE_WARN ) {
			_WARNINGLOG( QString( "archive_read_extract() [%1] %2" )
						 .arg( archive_errno( arch ) )
						 .arg( archive_error_string( arch ) ) );
		} else if ( r != ARCHIVE_OK ) {
			_ERRORLOG( QString( "archive_read_extract() [%1] %2" )
					   .arg( archive_errno( arch ) )
					   .arg( archive_error_string( arch ) ) );
			ret = false;
			break;
		}
	}
	archive_read_close( arch );

	if ( pInstalledPath != nullptr ) {
		*pInstalledPath = sExtractedDir;
	}

#if ARCHIVE_VERSION_NUMBER < 3000000
	archive_read_finish( arch );
#else
	archive_read_free( arch );
#endif

	return ret;
#else // H2CORE_HAVE_LIBARCHIVE
#ifndef WIN32
	// GUNZIP
	
	QString gzd_name = sSourcePath.left( sSourcePath.indexOf( "." ) ) + ".tar";
	FILE* gzd_file = fopen( gzd_name.toLocal8Bit(), "wb" );
	gzFile gzip_file = gzopen( sSourcePath.toLocal8Bit(), "rb" );
	if ( !gzip_file ) {
		_ERRORLOG( QString( "Error reading drumkit file: %1" )
				   .arg( sSourcePath ) );
		gzclose( gzip_file );
		fclose( gzd_file );
		return false;
	}
	uchar buf[4096];
	while ( gzread( gzip_file, buf, 4096 ) > 0 ) {
		fwrite( buf, sizeof( uchar ), 4096, gzd_file );
	}
	gzclose( gzip_file );
	fclose( gzd_file );
	// UNTAR
	TAR* tar_file;

	QByteArray tar_path = gzd_name.toLocal8Bit();

	if ( tar_open( &tar_file, tar_path.data(), NULL, O_RDONLY, 0,  TAR_GNU ) == -1 ) {
		_ERRORLOG( QString( "tar_open(): %1" ).arg( QString::fromLocal8Bit( strerror( errno ) ) ) );
		return false;
	}
	bool ret = true;
	char dst_dir[1024];

	QString dk_dir;
	if ( ! sTargetPath.isEmpty() ) {
		dk_dir = sTargetPath + "/";
	} else {
		dk_dir = Filesystem::usr_drumkits_dir() + "/";
	}

	strncpy( dst_dir, dk_dir.toLocal8Bit(), 1024 );
	if ( tar_extract_all( tar_file, dst_dir ) != 0 ) {
		_ERRORLOG( QString( "tar_extract_all(): %1" )
				   .arg( QString::fromLocal8Bit( strerror( errno ) ) ) );
		ret = false;
	}
	if ( tar_close( tar_file ) != 0 ) {
		_ERRORLOG( QString( "tar_close(): %1" )
				   .arg( QString::fromLocal8Bit( strerror( errno ) ) ) );
		ret = false;
	}
	return ret;
#else // WIN32
	_ERRORLOG( "WIN32 NOT IMPLEMENTED" );
	return false;
#endif
#endif
}

bool Drumkit::exportTo( const QString& sTargetDir, int nComponentId,
						bool bRecentVersion, bool bSilent ) {

	if ( ! Filesystem::path_usable( sTargetDir, true, false ) ) {
		ERRORLOG( QString( "Provided destination folder [%1] is not valid" )
				  .arg( sTargetDir ) );
		return false;
	}

	if ( ! bRecentVersion && nComponentId == -1 ) {
		ERRORLOG( "A DrumkitComponent ID is required to exported a drumkit in a format similar to the one prior to version 0.9.7" );
		return false;
	}

	if ( ! Filesystem::dir_readable( m_sPath, true ) ) {
		ERRORLOG( QString( "Unabled to access folder associated with drumkit [%1]" )
				  .arg( m_sPath ) );
		return false;
	}

	// Retrieve the component's name.
	auto componentLabels = generateUniqueComponentLabels();
	const QString sComponentName = componentLabels[ nComponentId ];

	// When performing an export of a single component, the resulting
	// file will be <DRUMKIT_NAME>_<COMPONENT_NAME>.h2drumkit. This
	// itself is nice because the user can not choose the name of the
	// resulting file and it would not be possible to store the export
	// of multiple components in a single folder otherwise. But if all
	// those different .h2drumkit would be extracted into the same
	// folder there would be easily confusion or maybe even loss of
	// data. We thus temporary rename the drumkit within this
	// function.
	// If a legacy export is asked for (!bRecentVersion) the suffix
	// "_legacy" will be appended as well in order to provide unique
	// filenames for all export options of a drumkit that can be
	// selected in the GUI.
	QString sOldDrumkitName = m_sName;
	QString sDrumkitName = getExportName( sComponentName, bRecentVersion );
	
	QString sTargetName = sTargetDir + "/" + sDrumkitName +
		Filesystem::drumkit_ext;
	
	if ( ! bSilent ) {
		QString sMsg( "Export ");
		
		if ( nComponentId == -1 && bRecentVersion ) {
			sMsg.append( "drumkit " );
		} else {
			sMsg.append( QString( "component: [%1|%2] " )
						 .arg( nComponentId ).arg( sComponentName ) );
		}

		sMsg.append( QString( "to [%1] " ).arg( sTargetName ) );

		if ( bRecentVersion ) {
			sMsg.append( "using the most recent format" );
		} else {
			sMsg.append( "using the legacy format supported by Hydrogen versions <= 0.9.6" );
		}

		INFOLOG( sMsg );
	}
	
	// Unique temporary folder to save intermediate drumkit.xml and
	// component files. The uniqueness is required in case several
	// threads or instances of Hydrogen do export a drumkit at once.
	QTemporaryDir tmpFolder( Filesystem::tmp_dir() + "/XXXXXX" );
	if ( nComponentId != -1 ) {
		tmpFolder.setAutoRemove( false );
	}

	// In case we just export a single component, we store a pruned
	// version of the drumkit with all other DrumkitComponents removed
	// from the Instruments in a temporary folder and use this one as
	// a basis for further compression.
	if ( nComponentId != -1 ) {
		if ( ! save( tmpFolder.path(), nComponentId, bRecentVersion, bSilent ) ) {
			ERRORLOG( QString( "Unable to save backup drumkit to [%1] using component [%2|%3]" )
					  .arg( tmpFolder.path() ).arg( nComponentId )
					  .arg( sComponentName ) );
		}
	}

	QDir sourceDir( m_sPath );

	QStringList sourceFilesList = sourceDir.entryList( QDir::Files );
	// In case just a single component is exported, we only add
	// samples associated with it to the .h2drumkit file.
	QStringList filesUsed;

	// List of formats libsndfile is able to import (see
	// https://libsndfile.github.io/libsndfile/api.html#open).
	// This list is used to decide what will happen to a file on a
	// single-component export in case the file is not associated with
	// a sample of an instrument belonging to the exported
	// component. If its suffix is contained in this list, the file is
	// considered to be part of an instrument we like to drop. If not,
	// it might be a metafile, like LICENSE, README, or the kit's
	// image.
	// The list does not have to be comprehensive as a "leakage" of
	// audio files in the resulting .h2drumkit is not a big problem.
	QStringList suffixBlacklist;
	suffixBlacklist << "wav" << "flac" << "aifc" << "aif" << "aiff" << "au"
					 << "caf" << "w64" << "ogg" << "pcm" << "l16" << "vob"
					 << "mp1" << "mp2" << "mp3";

	// We won't copy the original drumkit map (fallback) neither but write the
	// current user-level one.
	suffixBlacklist << "h2map";

	bool bSampleFound;
	
	for ( const auto& ssFile : sourceFilesList ) {
		if( ssFile.compare( Filesystem::drumkit_xml() ) == 0 &&
			nComponentId != -1 ) {
			filesUsed << Filesystem::drumkit_file( tmpFolder.path() );
		} else {

			bSampleFound = false;
			for( const auto& pInstr : *( getInstruments() ) ){
				if( pInstr != nullptr ) {
					for ( auto const& pComponent : *( pInstr->get_components() ) ) {
						if ( pComponent != nullptr &&
							 ( nComponentId == -1 ||
							   pComponent->get_drumkit_componentID() == nComponentId ) ) {
							for( int n = 0; n < InstrumentComponent::getMaxLayers(); n++ ) {
								const auto pLayer = pComponent->get_layer( n );
								if( pLayer != nullptr && pLayer->get_sample() != nullptr ) {
									if( pLayer->get_sample()->get_filename().compare( ssFile ) == 0 ) {
										filesUsed << sourceDir.filePath( ssFile );
										bSampleFound = true;
										break;
									}
								}
							}
						}
					}
				}
			}

			// Should we drop the file?
			if ( ! bSampleFound ) {
				QFileInfo ffileInfo( sourceDir.filePath( ssFile ) );
				if ( ! suffixBlacklist.contains( ffileInfo.suffix(),
												 Qt::CaseInsensitive ) ) {

					// We do not want to export any old backups created during
					// the upgrade process of the drumkits. As these were
					// introduced using suffixes, like .bak.1, .bak.2, etc,
					// adding `bak` to the blacklist will not work.
					if ( ! ( ssFile.contains( Filesystem::drumkit_xml() ) &&
							 ssFile.contains( ".bak" ) ) ) {
						filesUsed << sourceDir.filePath( ssFile );
					}
				}
			}
		}
	}

	// Store a copy of the current drumkit map and add it to the included files.
	QString sTmpMapPath = tmpFolder.filePath( QString( "%1%2" )
									 .arg( sDrumkitName )
									 .arg( Filesystem::drumkit_map_ext ) );
	m_pDrumkitMap->save( sTmpMapPath );
	filesUsed << sTmpMapPath;

#if defined(H2CORE_HAVE_LIBARCHIVE)

	struct archive *a;
	struct archive_entry *entry;
	struct stat st;
	char buff[8192];
	int len;
	FILE *f;

	a = archive_write_new();

	#if ARCHIVE_VERSION_NUMBER < 3000000
		archive_write_set_compression_gzip(a);
	#else
		archive_write_add_filter_gzip(a);
	#endif

	archive_write_set_format_pax_restricted(a);
	
	int ret = archive_write_open_filename(a, sTargetName.toUtf8().constData());
	if ( ret != ARCHIVE_OK ) {
		ERRORLOG( QString("Couldn't create archive [%0]" )
			.arg( sTargetName ) );
		setName( sOldDrumkitName );
		return false;
	}

	bool bFoundFileInRightComponent;
	for ( const auto& sFilename : filesUsed ) {
		QFileInfo ffileInfo( sFilename );
		QString sTargetFilename = sDrumkitName + "/" + ffileInfo.fileName();

		// Small sanity check since the libarchive code won't fail
		// gracefully but segfaults if the provided file does not
		// exist.
		if ( ! Filesystem::file_readable( sFilename, true ) ) {
			ERRORLOG( QString( "Unable to export drumkit. File [%1] does not exists or is not readable." )
					  .arg( sFilename ) );
			setName( sOldDrumkitName );
			return false;
		}

		stat( sFilename.toUtf8().constData(), &st );
		entry = archive_entry_new();
		archive_entry_set_pathname(entry, sTargetFilename.toUtf8().constData());
		archive_entry_set_size(entry, st.st_size);
		archive_entry_set_filetype(entry, AE_IFREG);
		archive_entry_set_perm(entry, 0644);
		archive_write_header(a, entry);
		f = fopen( sFilename.toUtf8().constData(), "rb" );
		len = fread(buff, sizeof(char), sizeof(buff), f);
		while ( len > 0 ) {
				archive_write_data(a, buff, len);
				len = fread(buff, sizeof(char), sizeof(buff), f);
		}
		fclose(f);
		archive_entry_free(entry);
	}
	archive_write_close(a);

	#if ARCHIVE_VERSION_NUMBER < 3000000
		archive_write_finish(a);
	#else
		archive_write_free(a);
	#endif

	sourceFilesList.clear();

	// Only clean up the temp folder when everything was
	// working. Else, it's probably worth inspecting its content (and
	// the system will clean it up anyway).
	Filesystem::rm( tmpFolder.path(), true, true );
	
	setName( sOldDrumkitName );

	return true;
#else // No LIBARCHIVE

#ifndef WIN32
	if ( nComponentId != -1 ) {
		// In order to add components name to the folder name we have
		// to copy _all_ files to a temporary folder holding the same
		// name. This is unarguably a quite expensive operation. But
		// exporting is only down sparsely and almost all versions of
		// Hydrogen should come with libarchive support anyway. On the
		// other hand, being consistent and prevent confusion and loss
		// of data beats sparsely excessive copying.
		QString sDirName = getFolderName();

		QDir sTmpSourceDir( tmpFolder.path() + "/" + sDirName );
		if ( sTmpSourceDir.exists() ) {
			sTmpSourceDir.removeRecursively();
		}
		if ( ! Filesystem::path_usable( tmpFolder.path() + "/" + sDirName,
									  true, true ) ) {
			ERRORLOG( QString( "Unable to create tmp folder [%1]" )
					  .arg( tmpFolder.path() + "/" + sDirName ) );
			set_name( sOldDrumkitName );
			return false;
		}

		QString sNewFilePath;
		QStringList copiedFiles;
		for ( const auto& ssFile : filesUsed ) {
			QString sNewFilePath( ssFile );
			sNewFilePath.replace( sNewFilePath.left( sNewFilePath.lastIndexOf( "/" ) ),
								  tmpFolder.path() + "/" + sDirName );
			if ( ! Filesystem::file_copy( ssFile, sNewFilePath, true, true ) ) {
				ERRORLOG( QString( "Unable to copy file [%1] to [%2]." )
						  .arg( ssFile ).arg( sNewFilePath ) );
				set_name( sOldDrumkitName );
				return false;
			}

			copiedFiles << sNewFilePath;
		}

		filesUsed = copiedFiles;
		sourceDir = QDir( tmpFolder.path() + "/" + sDirName );
	}

	// Since there is no way to alter the target names of the files
	// provided to command line `tar` and we want the output to be
	// identically to the only created used libarchive, we need to do
	// some string replacement in here. If not, the unpack to
	// ./home/USER_NAME_RUNNING_THE_EXPORT/.hydrogen/data/drumkits/DRUMKIT_NAME/
	// but we instead want it to unpack to ./DRUMKIT_NAME/
	filesUsed = filesUsed.replaceInStrings( sourceDir.absolutePath(),
											sourceDir.dirName() );

	QString sCmd = QString( "tar czf %1 -C %2 -- \"%3\"" )
		.arg( sTargetName )
		.arg( sourceDir.absolutePath().left( sourceDir.absolutePath().lastIndexOf( "/" ) ) )
		.arg( filesUsed.join( "\" \"" ) );
	int nRet = std::system( sCmd.toLocal8Bit() );

	if ( nRet != 0 ) {
		ERRORLOG( QString( "Unable to export drumkit using system command:\n%1" )
				  .arg( sCmd ) );
			set_name( sOldDrumkitName );
		return false;
	}

	// Only clean up the temp folder when everything was
	// working. Else, it's probably worth inspecting its content (and
	// the system will clean it up anyway).
	Filesystem::rm( tmpFolder.path(), true, true );

	set_name( sOldDrumkitName );
			
	return true;
#else // WIN32
	ERRORLOG( "Operation not supported on Windows" );
	
	return false;
#endif
#endif // LIBARCHIVE

}

QString Drumkit::getPath() const {
	return m_sPath;
}

std::shared_ptr<DrumkitComponent> Drumkit::getComponent( int nID ) const
{
	for ( auto pComponent : *m_pComponents ) {
		if ( pComponent->get_id() == nID ) {
			return pComponent;
		}
	}

	return nullptr;
}

std::map<int,QString> Drumkit::generateUniqueComponentLabels() const {
	std::map<int, QString> labelMap;

	QStringList uniqueLabels;
	for ( const auto& ppComponent : *m_pComponents ) {
		if ( ppComponent != nullptr ) {
			const auto sName = ppComponent->get_name();
			const int nId = ppComponent->get_id();
			if ( uniqueLabels.contains( sName ) ) {
				labelMap[ nId ] = QString( "%1 (%2)" ).arg( sName ).arg( nId );
			}
			else {
				labelMap[ nId ] = sName;
				uniqueLabels << sName;
			}
		}
	}

	return labelMap;
}

void Drumkit::recalculateRubberband( float fBpm ) {

	if ( !Preferences::get_instance()->getRubberBandBatchMode() ) {
		return;
	}

	if ( m_pInstruments != nullptr ) {
		for ( unsigned nnInstr = 0; nnInstr < m_pInstruments->size(); ++nnInstr ) {
			auto pInstr = m_pInstruments->get( nnInstr );
			if ( pInstr == nullptr ) {
				continue;
			}
			if ( pInstr != nullptr ){
				for ( int nnComponent = 0; nnComponent < pInstr->get_components()->size();
					  ++nnComponent ) {
					auto pInstrumentComponent = pInstr->get_component( nnComponent );
					if ( pInstrumentComponent == nullptr ) {
						continue; // regular case when you have a new component empty
					}

					for ( int nnLayer = 0; nnLayer < InstrumentComponent::getMaxLayers(); nnLayer++ ) {
						auto pLayer = pInstrumentComponent->get_layer( nnLayer );
						if ( pLayer != nullptr ) {
							auto pSample = pLayer->get_sample();
							if ( pSample != nullptr ) {
								if( pSample->get_rubberband().use ) {
									auto pNewSample = std::make_shared<Sample>( pSample );

									if ( ! pNewSample->load( fBpm ) ){
										continue;
									}

									// insert new sample from newInstrument
									pLayer->set_sample( pNewSample );
								}
							}
						}
					}
				}
			}
		}
	} else {
		ERRORLOG( "No InstrumentList present" );
	}
}

Drumkit::Type Drumkit::DetermineType( const QString& sPath ) {
	if ( ! sPath.isEmpty() ) {
		const QString sAbsolutePath = Filesystem::absolute_path( sPath );
		if ( sAbsolutePath.contains( Filesystem::sys_drumkits_dir() ) ) {
			return Type::System;
		}
		else if ( sAbsolutePath.contains( Filesystem::usr_drumkits_dir() ) ) {
			return Type::User;
		}
		else {
			if ( Filesystem::dir_writable( sAbsolutePath, true ) ) {
				return Type::SessionReadWrite;
			} else {
				return Type::SessionReadOnly;
			}
		}
	} else {
		return Type::Song;
	}
}

QString Drumkit::TypeToString( Type type ) {
	switch( type ) {
	case Type::System:
		return "System";
	case Type::User:
		return "User";
	case Type::SessionReadOnly:
		return "SessionReadOnly";
	case Type::SessionReadWrite:
		return "SessionReadWrite";
	case Type::Song:
		return "Song";
	default:
		return QString( "Unknown type [%1]" ).arg( static_cast<int>(type) );
	}
}

QString Drumkit::toQString( const QString& sPrefix, bool bShort ) const {
	QString s = Base::sPrintIndention;
	QString sOutput;
	if ( ! bShort ) {
		sOutput = QString( "%1[Drumkit]\n" ).arg( sPrefix )
			.append( QString( "%1%2type: %3\n" ).arg( sPrefix ).arg( s )
					 .arg( TypeToString( m_type ) ) )
			.append( QString( "%1%2path: %3\n" ).arg( sPrefix ).arg( s ).arg( m_sPath ) )
			.append( QString( "%1%2name: %3\n" ).arg( sPrefix ).arg( s ).arg( m_sName ) )
			.append( QString( "%1%2author: %3\n" ).arg( sPrefix ).arg( s ).arg( m_sAuthor ) )
			.append( QString( "%1%2info: %3\n" ).arg( sPrefix ).arg( s ).arg( m_sInfo ) )
			.append( QString( "%1%2license: %3\n" ).arg( sPrefix ).arg( s ).arg( m_license.toQString() ) )
			.append( QString( "%1%2image: %3\n" ).arg( sPrefix ).arg( s ).arg( m_sImage ) )
			.append( QString( "%1%2imageLicense: %3\n" ).arg( sPrefix ).arg( s ).arg( m_imageLicense.toQString() ) )
			.append( QString( "%1%2samples_loaded: %3\n" ).arg( sPrefix ).arg( s ).arg( m_bSamplesLoaded ) )
			.append( QString( "%1" ).arg( m_pInstruments->toQString( sPrefix + s, bShort ) ) )
			.append( QString( "%1%2components:\n" ).arg( sPrefix ).arg( s ) );
		for ( auto cc : *m_pComponents ) {
			if ( cc != nullptr ) {
				sOutput.append( QString( "%1" ).arg( cc->toQString( sPrefix + s + s, bShort ) ) );
			}
		}
		sOutput.append( QString( "%1%2m_pDrumkitMap: %3" ).arg( sPrefix ).arg( s )
						.arg( m_pDrumkitMap->toQString( sPrefix + s, bShort ) ) )
			.append( QString( "%1%2m_pDrumkitMapFallback: %3" ).arg( sPrefix ).arg( s )
						.arg( m_pDrumkitMapFallback->toQString( sPrefix + s, bShort ) ) );

	} else {
		
		sOutput = QString( "[Drumkit]" )
			.append( QString( " type: %1" ).arg( TypeToString( m_type ) ) )
			.append( QString( ", path: %1" ).arg( m_sPath ) )
			.append( QString( ", name: %1" ).arg( m_sName ) )
			.append( QString( ", author: %1" ).arg( m_sAuthor ) )
			.append( QString( ", info: %1" ).arg( m_sInfo ) )
			.append( QString( ", license: %1" ).arg( m_license.toQString() ) )
			.append( QString( ", image: %1" ).arg( m_sImage ) )
			.append( QString( ", imageLicense: %1" ).arg( m_imageLicense.toQString() ) )
			.append( QString( ", samples_loaded: %1" ).arg( m_bSamplesLoaded ) )
			.append( QString( ", [%1]" ).arg( m_pInstruments->toQString( sPrefix + s, bShort ) ) )
			.append( QString( ", components: [ " ) );
		for ( auto cc : *m_pComponents ) {
			if ( cc != nullptr ) {
				sOutput.append( QString( "[%1]" ).arg( cc->toQString( sPrefix + s + s, bShort ).replace( "\n", " " ) ) );
			}
		}
		sOutput.append( QString( ", [m_pDrumkitMap: %1]" )
						.arg( m_pDrumkitMap->toQString( sPrefix + s, bShort ) ) )
			.append( QString( ", [m_pDrumkitMapFallback: %1]" )
						.arg( m_pDrumkitMapFallback->toQString( sPrefix + s, bShort ) ) )
			.append( "]\n" );
	}
	
	return sOutput;
}

};

/* vim: set softtabstop=4 noexpandtab: */
