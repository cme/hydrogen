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

#include "DrumkitExportDialog.h"
#include "../HydrogenApp.h"
#include "../CommonStrings.h"
#include "../Widgets/FileDialog.h"

#include <core/Helpers/Filesystem.h>
#include <core/Preferences/Preferences.h>
#include <core/Basics/DrumkitComponent.h>

using namespace H2Core;

DrumkitExportDialog::DrumkitExportDialog( QWidget* pParent,
													std::shared_ptr<Drumkit> pDrumkit )
	: QDialog( pParent ),
	  m_pDrumkit( pDrumkit )
{
	auto pCommonStrings = HydrogenApp::get_instance()->getCommonStrings();
	
	setupUi( this );

	exportBtn->setText( tr( "&Export" ) );
	cancelBtn->setText( pCommonStrings->getButtonCancel() );
	
	setWindowTitle( QString( "%1 [%2]" )
					.arg( tr( "Export Drumkit" ) )
					.arg( pDrumkit != nullptr ? pDrumkit->getName() : tr( "invalid drumkit" ) ) );
	adjustSize();
	setFixedSize( width(), height() );
	drumkitPathTxt->setText( Preferences::get_instance()->getLastExportDrumkitDirectory() );

	if ( pDrumkit != nullptr ) {
		m_componentLabels = pDrumkit->generateUniqueComponentLabels();
		updateComponentList();
	}
}

DrumkitExportDialog::~DrumkitExportDialog()
{
}



void DrumkitExportDialog::on_exportBtn_clicked()
{
	if ( m_pDrumkit == nullptr ) {
		ERRORLOG( "Invalid drumkit" );
		return;
	}

	auto pCommonStrings = HydrogenApp::get_instance()->getCommonStrings();

	if ( ! Filesystem::dir_writable( drumkitPathTxt->text(), false ) ) {
		QMessageBox::warning( this, "Hydrogen",
							  pCommonStrings->getFileDialogMissingWritePermissions(),
							  QMessageBox::Ok );
		return;
	}


	if ( ! HydrogenApp::checkDrumkitLicense( m_pDrumkit ) ) {
		ERRORLOG( "User cancelled dialog due to licensing issues." );
		return;
	}
		
	bool bRecentVersion = versionList->currentIndex() == 1 ? false : true;

	QString sTargetComponentName;
	if ( componentList->currentIndex() == 0 && bRecentVersion ) {
		// Exporting all components
		sTargetComponentName = "";
	} else {
		sTargetComponentName = componentList->currentText();
	}

	// Retrieve the Id of the selected component
	int nTargetComponentId = -1;
	if ( ! sTargetComponentName.isEmpty() ) {
		// These are unique
		for ( const auto& [ nnId, ssLabel ] : m_componentLabels ) {
			if ( ssLabel == sTargetComponentName ) {
				nTargetComponentId = nnId;
				break;
			}
		}

		if ( nTargetComponentId == -1 ) {
			ERRORLOG( QString( "No ID could be retrieved for component [%1]" )
					  .arg( sTargetComponentName ) );
			QMessageBox::critical( this, "Hydrogen",
								   pCommonStrings->getExportDrumkitFailure() );
			return;
		}
	}
		
	// Check whether the resulting file does already exist and ask the
	// user if it should be overwritten.
	QString sTargetName = drumkitPathTxt->text() + "/" +
		m_pDrumkit->getExportName( sTargetComponentName, bRecentVersion ) +
		Filesystem::drumkit_ext;
	
	if ( Filesystem::file_exists( sTargetName, true ) ) {
		QMessageBox msgBox;
		msgBox.setWindowTitle("Hydrogen");
		msgBox.setIcon( QMessageBox::Warning );
		msgBox.setText( tr( "The file [%1] does already exist and will be overwritten.")
						.arg( sTargetName ) );

		msgBox.setStandardButtons( QMessageBox::Ok | QMessageBox::Cancel );
		msgBox.setButtonText(QMessageBox::Ok,
							 pCommonStrings->getButtonOk() );
		msgBox.setButtonText(QMessageBox::Cancel,
							 pCommonStrings->getButtonCancel());
		msgBox.setDefaultButton(QMessageBox::Ok);

		if ( msgBox.exec() == QMessageBox::Cancel ) {
			return;
		}
	}
	
	QApplication::setOverrideCursor(Qt::WaitCursor);
	
	if ( ! m_pDrumkit->exportTo( drumkitPathTxt->text(), // Target folder
								 nTargetComponentId, // Selected component
								 bRecentVersion ) ) {
		QApplication::restoreOverrideCursor();
		QMessageBox::critical( this, "Hydrogen",
							   pCommonStrings->getExportDrumkitFailure() );
		return;
	}

	QApplication::restoreOverrideCursor();
	QMessageBox::information( this, "Hydrogen",
							  tr("Drumkit exported to") + "\n" +
							  sTargetName );
}

void DrumkitExportDialog::on_drumkitPathTxt_textChanged( QString str )
{
	QString path = drumkitPathTxt->text();
	if (path.isEmpty()) {
		exportBtn->setEnabled( false );
	}
	else {
		exportBtn->setEnabled( true );
	}
}

void DrumkitExportDialog::on_browseBtn_clicked()
{
	QString sPath = Preferences::get_instance()->getLastExportDrumkitDirectory();
	if ( ! Filesystem::dir_writable( sPath, false ) ){
		sPath = QDir::homePath();
	}

	FileDialog fd(this);
	fd.setFileMode( QFileDialog::Directory );
	fd.setAcceptMode( QFileDialog::AcceptSave );
	fd.setDirectory( sPath );
	fd.setWindowTitle( tr("Directory") );

	if ( fd.exec() == QDialog::Accepted ) {
		QString sFilename = fd.selectedFiles().first();
		if ( sFilename.isEmpty() ) {
			drumkitPathTxt->setText( sPath );
		} else {
			drumkitPathTxt->setText( sFilename );
			Preferences::get_instance()->setLastExportDrumkitDirectory( sFilename );
		}
	}
}

void DrumkitExportDialog::on_cancelBtn_clicked()
{
	accept();
}

void DrumkitExportDialog::on_versionList_currentIndexChanged( int index )
{
	updateComponentList();
}

void DrumkitExportDialog::updateComponentList( )
{
	componentList->clear();

	if ( versionList->currentIndex() == 0 ) {
		// Only kit version 0.9.7 or newer support components. For
		// them we can support to export all or individual ones. For
		// older versions one component must pretend to be the whole
		// kit.
		componentList->addItem( tr( "All" ) );
		componentList->insertSeparator( 1 );
	}

	for ( const auto& [ nnId, ssLabel ] : m_componentLabels ) {
		componentList->addItem( ssLabel );
	}
}
