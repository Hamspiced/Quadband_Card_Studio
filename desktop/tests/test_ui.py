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
    import asyncio
    asyncio.run_coroutine_threadsafe(w.service.close(), w.worker.loop).result(timeout=3)
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


def test_bluetooth_refresh_clears_old_peers_and_requires_explicit_selection(window, monkeypatch):
    connection = window.lf_connection
    connection.mode.setCurrentText('Bluetooth')
    connection.device.addItem('Stale peer', 'old')
    monkeypatch.setattr(window.worker, 'submit', lambda factory: None)
    connection.refresh()
    assert connection.device.count() == 0
    assert connection.discovered_at is None
    window.done([('one', 'First candidate'), ('two', 'Second candidate')])
    assert connection.device.count() == 2
    assert connection.device.currentData() is None
    connection.device.setCurrentIndex(1)
    assert connection.device.currentData() == 'two'
    connection.discovered_at -= 31
    connection.connect()
    assert 'expired' in window.status.text()
    assert connection.device.count() == 0
    assert not window.busy


def test_idle_bluetooth_loss_removes_shared_hf_reader_and_unlocks_transport(window):
    connection = window.lf_connection
    connection.mode.setCurrentText('Bluetooth')
    connection.discovered([('peer', 'Selected reader')])
    connection.device.setCurrentIndex(0)
    connection.connected('Validated Chameleon')
    window.hf_readers.setCurrentText('Chameleon Ultra · connected')
    assert not connection.mode.isEnabled()
    window.reader_disconnected('chameleon', 'Bluetooth disconnected. Scan and reconnect.')
    assert connection.status.text().startswith('Bluetooth disconnected')
    assert window.hf_readers.findText('Chameleon Ultra · connected') == -1
    assert window.hf_readers.currentIndex() == -1
    assert not window.hf_read_button.isEnabled()
    assert not window.hf_write_records_button.isEnabled()
    assert connection.mode.isEnabled() and connection.device.count() == 0
    assert not connection.linked


def test_bluetooth_pairing_cancel_does_not_leave_a_ready_reader(window, app, monkeypatch):
    import asyncio
    from quadband import transports
    entered = __import__('threading').Event()
    async def pairing(*args, **kwargs):
        window.worker.progress.emit('Pairing Bluetooth; complete the system prompt.')
        entered.set()
        await asyncio.Event().wait()
    monkeypatch.setattr(transports.BLEChannel, 'connect', pairing)
    connection = window.lf_connection
    connection.mode.setCurrentText('Bluetooth')
    connection.discovered([('peer', 'Selected reader')])
    connection.device.setCurrentIndex(0)
    connection.connect()
    assert entered.wait(2)
    app.processEvents()
    assert 'Pairing' in window.status.text()
    window.stop()
    wait_idle(window, app)
    assert window.service.chameleon is None
    assert not connection.linked
    assert window.hf_readers.findText('Chameleon Ultra · connected') == -1
    assert 'stopped' in window.status.text()


@pytest.mark.parametrize('mode,pending', [('home', False), ('standalone', False), ('connected', True)])
def test_bridge_modes_remove_readers_and_clear_authentication(window, mode, pending):
    import copy
    from quadband import cores3
    from test_cores3 import GOLDEN
    value = copy.deepcopy(GOLDEN['capabilities'])
    window.bridge_changed(value)
    window.hf_readers.setCurrentText(cores3.HF_READER)
    window.ntag_password.setText('01020304')
    window.ntag_pack.setText('1234')
    value = copy.deepcopy(value)
    value.update(mode=mode, owner={'home': 'none', 'standalone': 'local', 'connected': 'desktop'}[mode],
                 modePending=pending)
    window.bridge_changed(value)
    assert window.hf_readers.findText(cores3.HF_READER) == -1
    assert not window.ntag_password.isEnabled() and not window.ntag_password.text()
    assert not window.ntag_pack.text() and not window.hf_read_button.isEnabled()
    assert not window.hf_write_records_button.isEnabled()
    assert window.cores3_connection.status.text() == cores3.mode_description(value)
    value = copy.deepcopy(value)
    value.update(mode='connected', owner='desktop', modePending=False)
    window.bridge_changed(value)
    assert window.hf_readers.findText(cores3.HF_READER) >= 0
    assert window.hf_readers.currentIndex() == -1


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


@pytest.mark.parametrize('profile', ['M5Stack JRD-4035', 'YRM100'])
def test_uhf_module_profiles_expose_only_supported_controls(window, profile):
    connection = window.uhf_connection
    connection.reader_profile.setCurrentText(profile)
    assert connection.baud.currentText() == '115200'
    assert not connection.address.isEnabled()
    assert window.uhf_word.maximum() == 65535
    assert window.uhf_power.minimum() == 18 and window.uhf_power.maximum() == 26
    assert all(not b.isEnabled() for b in window.uhf_buzzer_buttons)
    connection.reader_profile.setCurrentText('Piswords UHF2')
    assert connection.baud.currentText() == '9600' and connection.address.isEnabled()
    assert window.uhf_word.maximum() == 2147483647
    assert all(b.isEnabled() for b in window.uhf_buzzer_buttons)


def test_uhf_module_reports_display_real_rssi_and_unknown_frequency(window):
    from quadband.m100 import parse_tag
    tag = parse_tag(bytes.fromhex('C9340030751FEB705C5904E3D50D703A76'))
    window.tag_received(tag)
    assert window.uhf_tags.item(0, 2).text() == 'Not reported'
    assert window.uhf_tags.item(0, 3).text() == '-55'


def wait_idle(window, app):
    import time
    from PySide6.QtTest import QTest
    deadline = time.monotonic() + 10
    while window.busy and time.monotonic() < deadline:
        app.processEvents()
        QTest.qWait(5)
    app.processEvents()
    assert not window.busy, window.status.text()


@pytest.mark.parametrize('family', ['classic4k', 'ntag213', 'ntag215', 'ntag216'])
def test_cores3_real_ui_read_data_write_records_and_profiles(window, app, monkeypatch, family):
    from quadband import cores3, hf
    from test_cores3 import TypedWire
    from test_hf_families import Classic4K, NTAG
    card = Classic4K(True) if family == 'classic4k' else NTAG(family)
    wire = TypedWire(card)
    async def connect():
        window.service.bridge = cores3.Session(wire, window.service.bridge_changed)
        await window.service.bridge.hello()
        return 'Simulated CoreS3 connected'
    window.run(connect)
    wait_idle(window, app)
    window.nav.setCurrentRow(3)
    assert window.sidebar.isAncestorOf(window.cores3_connection)
    assert window.hf_readers.findText(cores3.HF_READER) >= 0
    assert window.hf_readers.currentIndex() == -1
    window.hf_readers.setCurrentText(cores3.HF_READER)
    window.read_hf()
    wait_idle(window, app)
    assert window.snapshot is not None, window.status.text()
    assert window.block_table.rowCount() == card.info.unit_count
    assert window.block_number.maximum() == card.info.unit_count-1
    assert family in window.hf_info.toPlainText()
    assert window.hf_write_records_button.isEnabled()
    monkeypatch.setattr(window, 'review', lambda *_: True)
    unit = 128 if family == 'classic4k' else 6
    size = card.info.unit_size
    window.block_number.setValue(unit)
    window.block_data.setText('AA '*size)
    window.write_hf_block()
    wait_idle(window, app)
    assert card.writes and card.writes[-1] == (unit, b'\xaa'*size), window.status.text()
    # Restore an intact simulated message before exercising the record editor.
    if family == 'classic4k':
        restored = Classic4K(True)
        card.memory.update(restored.memory)
    else:
        restored = NTAG(family)
        card.pages.update(restored.pages)
    window.read_hf()
    wait_idle(window, app)
    window.records = [ndef.Record.text('Qt bridge write')]
    window.ndef_overwrite.setChecked(True)
    window.update_records()
    window.write_records()
    wait_idle(window, app)
    assert window.ndef_area is not None, window.status.text()
    assert window.ndef_area.records == window.records
    window.use_profile()
    assert window.profile['family'] == family
    assert len(hf.profile_blocks(window.profile)) == len(hf.eligible_units(family))
    assert window.hf_write_profile_button.isEnabled()
    window.write_profile()
    wait_idle(window, app)
    assert 'verified' in window.status.text().lower(), window.status.text()
    window.run(window.service.close)
    wait_idle(window, app)
    assert wire.closed


def test_bridge_generation_invalidates_preview_and_never_selects_replacement(window):
    import copy
    from quadband import cores3, hf
    from test_cores3 import GOLDEN
    value = copy.deepcopy(GOLDEN['capabilities'])
    window.bridge_changed(value)
    window.hf_readers.setCurrentText(cores3.HF_READER)
    window.snapshot = hf.Snapshot(hf.CardInfo(b'1234', atqa=b'\4\0', sak=8))
    window.ntag_password.setText('11223344')
    window.ntag_pack.setText('1234')
    assert window.authentication() == bytes.fromhex('02112233441234')
    value = copy.deepcopy(value)
    value['peripherals'][0]['generation'] += 1
    window.bridge_changed(value)
    assert window.snapshot is None and window.hf_readers.currentIndex() == -1
    assert not window.ntag_password.text() and not window.ntag_pack.text()
    assert not window.hf_write_block_button.isEnabled()
    window.bridge_changed(None)
    assert window.hf_readers.findText(cores3.HF_READER) < 0
    assert window.settings.allKeys() == []


def test_ndef_editor_bulk_limit_reorder_capacity_and_options_default_off(window):
    from quadband.common import CardError
    assert not window.ndef_overwrite.isChecked() and not window.ndef_format.isChecked()
    window.ndef_family.setCurrentIndex(window.ndef_family.findData('ntag213'))
    window.records = [ndef.Record.text(str(i)) for i in range(4)]
    window.update_records()
    assert '4 records' in window.record_size.text() and '142' in window.record_size.text()
    with pytest.raises(CardError, match='one to three'):
        window.ndef_job(True)
    assert len(window.ndef_job(False).records) == 4
    window.record_list.setCurrentRow(3)
    window.move_record(-1)
    assert window.records[2].display()[1] == '3'
    window.remove_record()
    assert len(window.records) == 3
    assert len(window.ndef_job(True).records) == 3
    window.ndef_overwrite.setChecked(True)
    window.ndef_format.setChecked(True)
    window.clear_session()
    assert not window.ndef_overwrite.isChecked() and not window.ndef_format.isChecked()
    assert window.settings.allKeys() == []


def test_ndef_review_default_is_cancel(window, monkeypatch):
    from PySide6.QtWidgets import QMessageBox
    seen = []
    def execute(box):
        seen.append(box.defaultButton())
        assert box.defaultButton() == box.button(QMessageBox.StandardButton.Cancel)
        return QMessageBox.StandardButton.Cancel
    monkeypatch.setattr(QMessageBox, 'exec', execute)
    assert not window.review('Target-bound review', 'One target only')
    assert seen


@pytest.mark.parametrize('accepted', [False, True])
def test_real_qt_bulk_requires_each_review_and_freezes_controls(window, app, monkeypatch, accepted):
    import asyncio
    from contextlib import asynccontextmanager
    from quadband import cores3, ntag
    from test_type2 import Physical
    from test_hf_families import NTAG
    card = NTAG('ntag215')
    backend = Physical(card)
    steps = iter([True, False, True, True, False, True, True])
    reviews = []
    async def present():
        value = next(steps, None)
        if value is None:
            window.stop_flag.set()
            return True
        return value
    backend.present = present
    @asynccontextmanager
    async def session(name, auth):
        yield cores3.HFClient(backend)
    def review(title, message):
        assert window.busy and not window.stack.isEnabled() and not window.connection_panel.isEnabled()
        assert 'UID 04010203040506' in message and 'ntag215' in message
        reviews.append(message)
        return accepted
    monkeypatch.setattr(window.service, 'hf_session', session)
    monkeypatch.setattr(window, 'review', review)
    window.hf_readers.addItem('Simulated PCSC')
    window.ndef_family.setCurrentIndex(window.ndef_family.findData('ntag215'))
    window.ndef_overwrite.setChecked(True)
    window.records = [ndef.Record.text('Bulk Qt')]
    window.update_records()
    window.start_ndef_bulk()
    wait_idle(window, app)
    assert len(reviews) == 2
    if accepted:
        assert card.writes and ntag.Type2NDEF(asyncio.run(ntag.Client(card).read())).records == window.records
        assert window.ndef_bulk_totals.text() == '2 verified · 0 failed'
    else:
        assert not card.writes and window.ndef_bulk_totals.text() == '0 verified · 0 failed'


@pytest.mark.parametrize('family', ['ntag213', 'ntag215', 'ntag216'])
def test_initialized_empty_ntag_first_record_ui(window, app, monkeypatch, family):
    from quadband import cores3, hf
    from test_cores3 import TypedWire
    from test_hf_families import NTAG
    card = NTAG(family)
    for page in hf.eligible_units(family):
        card.pages[page] = bytes(4)
    wire = TypedWire(card)
    async def connect():
        window.service.bridge = cores3.Session(wire, window.service.bridge_changed)
        await window.service.bridge.hello()
    window.run(connect)
    wait_idle(window, app)
    window.nav.setCurrentRow(3)
    window.hf_readers.setCurrentText(cores3.HF_READER)
    window.read_hf()
    wait_idle(window, app)
    assert window.ndef_area is not None and window.ndef_area.records == [], window.status.text()
    assert window.hf_write_records_button.isEnabled()
    monkeypatch.setattr(window, 'review', lambda *_: True)
    window.records = [ndef.Record.text('First message from the record editor')]
    window.update_records()
    window.write_records()
    wait_idle(window, app)
    assert window.ndef_area is not None and window.ndef_area.records == window.records, window.status.text()
    assert card.writes[0] == (4, bytes(4)) and card.writes[-1][0] == 4


def test_core_s3_uhf_selection_uses_module_limits_without_serial_port(window):
    window.uhf_connection.mode.setCurrentText('CoreS3')
    assert window.uhf_connection.reader_profile.currentText() == 'M5Stack JRD-4035'
    assert not window.uhf_connection.baud.isEnabled()
    assert window.uhf_power.minimum() == 18 and window.uhf_power.maximum() == 26
    assert not any(b.isEnabled() for b in window.uhf_buzzer_buttons)


def test_cores3_auth_capability_required_and_clear_session_forgets_credentials(window):
    import copy
    from quadband import cores3
    from test_cores3 import GOLDEN
    value = copy.deepcopy(GOLDEN['capabilities'])
    value['peripherals'][0]['capabilities'].remove('ntag_auth')
    window.bridge_changed(value)
    window.hf_readers.setCurrentText(cores3.HF_READER)
    assert not window.ntag_password.isEnabled() and window.authentication() == b'\0'
    value['peripherals'][0]['capabilities'].append('ntag_auth')
    value['peripherals'][0]['generation'] += 1
    window.bridge_changed(value)
    window.hf_readers.setCurrentText(cores3.HF_READER)
    window.ntag_password.setText('11223344')
    window.ntag_pack.setText('1234')
    window.clear_session()
    assert not window.ntag_password.text() and not window.ntag_pack.text()
