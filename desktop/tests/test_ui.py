import os
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
import pytest
from quadband.app import QApplication, MainWindow, RecordDialog
from quadband import ndef, magstripe


@pytest.fixture(scope='module')
def app():
    return QApplication.instance() or QApplication([])


@pytest.fixture
def window(app):
    w = MainWindow()
    yield w
    w.worker.loop.call_soon_threadsafe(w.worker.loop.stop)
    assert w.worker.wait(3000)
    w.closed_ready = True
    w.close()


def test_workspaces_and_msr_mapping(window, app):
    assert [window.nav.item(i).text() for i in range(6)] == [
        'Magstripe Write', 'Magstripe Read', 'HF Workspace', 'UHF Workspace', 'LF Workspace', 'About & help']
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
