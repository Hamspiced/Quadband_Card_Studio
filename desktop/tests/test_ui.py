import os
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
import pytest
from quadband.app import QApplication, MainWindow, RecordDialog, QSettings
from quadband import ndef, magstripe


@pytest.fixture(scope='module')
def app():
    return QApplication.instance() or QApplication([])


@pytest.fixture
def window(app, tmp_path):
    w = MainWindow(settings=QSettings(str(tmp_path / 'appearance.ini'), QSettings.Format.IniFormat))
    yield w
    w.worker.loop.call_soon_threadsafe(w.worker.loop.stop)
    assert w.worker.wait(3000)
    w.closed_ready = True
    w.close()


def test_workspaces_and_msr_mapping(window, app):
    assert [window.nav.item(i).text() for i in range(6)] == [
        'Magstripe Write', 'Magstripe Read', 'UHF Workspace', 'HF Workspace', 'LF Workspace', 'About & help']
    for i in range(6):
        window.nav.setCurrentRow(i)
        page = window.stack.currentWidget().widget()
        assert page.layout().itemAt(0).widget().text() == window.nav.item(i).text()
    window.msr_sample(True)
    assert [e.text() for e in window.tracks] == magstripe.SHORT
    window.swipe_mapping.setCurrentText('Tracks 2, 3')
    window.swipe_raw.setPlainText('123\t456\n')
    window.parse_swipe()
    assert '123' in window.swipe_result.toPlainText() and '456' in window.swipe_result.toPlainText()
    window.clear_session()
    assert not window.swipe_raw.toPlainText()
    assert window.profile is None and window.lf_card is None


def test_all_record_forms_are_constructible(window):
    dialog = RecordDialog(window)
    for kind in ndef.FIELDS:
        dialog.kind.setCurrentText(kind)
        assert set(dialog.inputs) == {field[0] for field in ndef.FIELDS[kind]}
    dialog.close()


def test_session_data_is_not_stored_in_settings(window):
    window.uhf_password.setText('AABBCCDD')
    window.clear_session()
    assert not hasattr(window, 'lf_old_password')
    assert not hasattr(window, 'lf_new_password')
    assert window.uhf_password.text() == '00000000'


def test_sidebar_connections_and_full_resolution_logo(window):
    assert window.sidebar.isAncestorOf(window.msr_connection)
    assert window.sidebar.isAncestorOf(window.lf_connection)
    assert window.msr_connection.mode.findText('Bluetooth') >= 0
    assert window.lf_connection.mode.findText('Bluetooth') >= 0
    assert window.logo.pixmap.width() == 1024
    assert window.logo.width() == 144
    assert not window.logo.grab().isNull()
    assert window.logo._rendered.width() == round(144 * window.logo.devicePixelRatioF())
    window.lf_connection.mode.setCurrentText('Bluetooth')
    window.lf_connection.device.addItem('Test Chameleon', 'test-device')
    for i in range(6):
        window.nav.setCurrentRow(i)
        assert window.lf_connection.device.currentData() == 'test-device'


def test_sidebar_connections_disabled_during_operations(window, monkeypatch):
    monkeypatch.setattr(window.worker, 'submit', lambda factory: None)
    window.run(lambda: None)
    assert not window.connection_panel.isEnabled()
    window.finish()
    assert window.connection_panel.isEnabled()


def test_appearance_modes_persist_without_card_data(window, app):
    from PySide6.QtGui import QPalette
    for mode, dark in [('Dark', True), ('Light', False)]:
        window.appearance.setCurrentText(mode)
        assert window.dark_mode is dark
        assert window.settings.value('appearance') == mode
        background = app.palette().color(QPalette.ColorRole.Window)
        assert (background.lightness() < 128) is dark
    window.appearance.setCurrentText('System')
    assert window.settings.value('appearance') == 'System'
    assert window.settings.allKeys() == ['appearance']
    window.settings.sync()
    restored = QSettings(window.settings.fileName(), QSettings.Format.IniFormat)
    assert restored.value('appearance') == 'System'
