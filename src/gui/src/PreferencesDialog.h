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

#ifndef PREFERENCES_DIALOG_H
#define PREFERENCES_DIALOG_H


#include <core/Object.h>
#include <QtWidgets>

///
/// Combo box showing a list of available devices for a given driver.
/// List is calculated lazily when needed.
///
class DeviceComboBox : public QComboBox {

	bool m_bHasDevices;
	QString m_sDriver;
	QString m_sHostAPI;

public:
	DeviceComboBox( QWidget *pParent );

	/// Set the driver name to use
	void setDriver( QString sDriver ) { m_sDriver = sDriver; }
	void setHostAPI( QString sHostAPI ) { m_sHostAPI = sHostAPI; }

	virtual void showPopup();
};

///
/// Combo box showing a list of HostAPIs.
///
class HostAPIComboBox : public QComboBox {

public:
	HostAPIComboBox( QWidget *pParent );
	void setValue( QString sHostAPI );
	virtual void showPopup();
};

#include "ui_PreferencesDialog_UI.h"

///
/// Preferences Dialog
///
class PreferencesDialog : public QDialog, private Ui_PreferencesDialog_UI, public H2Core::Object
{
	H2_OBJECT
	Q_OBJECT
	public:
		explicit PreferencesDialog( QWidget* parent );
		~PreferencesDialog();
		static QString m_sColorRed;
							  
	private slots:
		void on_okBtn_clicked();
		void on_cancelBtn_clicked();
		void on_selectApplicationFontBtn_clicked();
		void on_selectMixerFontBtn_clicked();
		void on_restartDriverBtn_clicked();
		void on_driverComboBox_activated( int index );
		void on_portaudioHostAPIComboBox_activated( int index );
		void on_bufferSizeSpinBox_valueChanged( int i );
		void on_resampleComboBox_currentIndexChanged ( int index );
		void on_sampleRateComboBox_editTextChanged( const QString& text );
		void on_midiPortComboBox_activated( int index );
		void on_midiOutportComboBox_activated( int index );		
		void on_styleComboBox_activated( int index );
		void on_useLashCheckbox_clicked();
		void onMidiDriverComboBoxIndexChanged( int index );
		void on_m_pAudioDeviceTxt_currentTextChanged( QString );
		void toggleTrackOutsCheckBox(bool toggled);
		void toggleOscCheckBox(bool toggled);
		void coloringMethodCombo_currentIndexChanged (int index);


	private:
		bool m_bNeedDriverRestart;
		QString m_sInitialLanguage;

		void updateDriverInfo();

		void updateDriverPreferences();
};


#endif

