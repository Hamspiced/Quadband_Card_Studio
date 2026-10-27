"""Portable Qt application. SPDX-License-Identifier: GPL-3.0-only"""
import asyncio
import csv
from datetime import datetime
from pathlib import Path
import sys
import threading
from PySide6.QtCore import Qt, QThread, Signal, QTimer, QSettings
from PySide6.QtGui import QPixmap, QIcon, QPainter, QPalette, QColor
from PySide6.QtWidgets import (QApplication, QMainWindow, QWidget, QHBoxLayout, QVBoxLayout, QFormLayout,
    QLabel, QPushButton, QComboBox, QLineEdit, QPlainTextEdit, QListWidget, QStackedWidget, QTabWidget,
    QGroupBox, QScrollArea, QSpinBox, QCheckBox, QMessageBox, QFileDialog, QDialog, QDialogButtonBox,
    QTableWidget, QTableWidgetItem, QHeaderView, QSizePolicy)
from . import __version__, magstripe, hf, ndef, chameleon, pcsc, reader_tools
from .common import CardError, require, hex_bytes, load_json, save_json
from .service import Service, StopFlag, call_sync

ASSETS = Path(__file__).parent / 'assets'


class Worker(QThread):
    progress = Signal(str)
    completed = Signal(object)
    failed = Signal(str)
    tag = Signal(object)
    batch = Signal(bool, str)

    def __init__(self):
        super().__init__()
        self.ready = threading.Event()
        self.loop = asyncio.new_event_loop()
        self.service = Service(self.progress.emit)
        self.future = None

    def run(self):
        asyncio.set_event_loop(self.loop)
        self.ready.set()
        self.loop.run_forever()
        self.loop.run_until_complete(self.loop.shutdown_asyncgens())
        self.loop.close()

    def submit(self, factory):
        async def perform():
            try:
                self.completed.emit(await factory())
            except asyncio.CancelledError:
                self.failed.emit('Operation stopped. A write already sent may have changed the card; read it before retrying.')
            except Exception as exc:
                message = str(exc) or type(exc).__name__
                if isinstance(exc, TimeoutError):
                    message = 'The reader timed out. Reconnect it; a write already sent may have changed the card.'
                self.failed.emit(message)
        self.future = asyncio.run_coroutine_threadsafe(perform(), self.loop)


def text_label(text, style=''):
    label = QLabel(text)
    label.setWordWrap(True)
    label.setTextFormat(Qt.TextFormat.PlainText)
    for color in ('#a7b7c3', '#91a9b7'):
        if f'color: {color};' in style:
            label.setProperty('tone', 'muted')
            style = style.replace(f'color: {color};', '')
    style = style.replace('color: #edf6f2;', '')
    if 'color: #ffa99b;' in style:
        label.setProperty('tone', 'error')
        style = style.replace('color: #ffa99b;', '')
    if style:
        label.setStyleSheet(style)
    return label


def button(text, action, primary=False):
    b = QPushButton(text.replace("&", "&&"))
    b.clicked.connect(action)
    if primary:
        b.setProperty('primary', True)
    return b


def row(*widgets):
    w = QWidget()
    layout = QHBoxLayout(w)
    layout.setContentsMargins(0, 0, 0, 0)
    for widget in widgets:
        layout.addWidget(widget)
    return w


def combo(items):
    c = QComboBox()
    if isinstance(items, dict):
        for title, value in items.items():
            c.addItem(title, value)
    else:
        c.addItems(items)
    return c


def edit(text='', secret=False):
    e = QLineEdit(text)
    if secret:
        e.setEchoMode(QLineEdit.EchoMode.Password)
    return e


def editor(readonly=False, height=110):
    e = QPlainTextEdit()
    e.setReadOnly(readonly)
    e.setMinimumHeight(height)
    e.setMaximumBlockCount(6000)
    return e


def group(title, *widgets):
    g = QGroupBox(title)
    layout = QVBoxLayout(g)
    for widget in widgets:
        layout.addWidget(widget)
    return g


def form_group(title, fields):
    g = QGroupBox(title)
    layout = QFormLayout(g)
    layout.setFieldGrowthPolicy(QFormLayout.FieldGrowthPolicy.AllNonFixedFieldsGrow)
    for title, widget in fields:
        layout.addRow(title, widget)
    return g


class BrandLogo(QWidget):
    def __init__(self):
        super().__init__()
        self.pixmap = QPixmap(str(ASSETS / 'MidwestGadgets.png'))
        self._render_key = None
        self._rendered = QPixmap()
        self.setFixedSize(144, 144)
        self.setAccessibleName('Midwest Gadgets logo')

    def paintEvent(self, event):
        ratio = self.devicePixelRatioF()
        key = (self.width(), self.height(), ratio)
        if key != self._render_key:
            self._rendered = self.pixmap.scaled(round(self.width() * ratio), round(self.height() * ratio),
                Qt.AspectRatioMode.KeepAspectRatio, Qt.TransformationMode.SmoothTransformation)
            self._rendered.setDevicePixelRatio(ratio)
            self._render_key = key
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
        target = self.rect()
        target.setSize(self.pixmap.size().scaled(self.size(), Qt.AspectRatioMode.KeepAspectRatio))
        target.moveCenter(self.rect().center())
        painter.drawPixmap(target, self._rendered)


class Connection(QWidget):
    def __init__(self, window, family, compact=False):
        super().__init__()
        self.window, self.family = window, family
        self.mode = combo(['USB serial', 'Bluetooth'] if family == 'chameleon' else ['USB serial'] if family == 'uhf' else ['USB HID', 'USB serial', 'Bluetooth'])
        self.device = QComboBox()
        self.device.setMinimumWidth(0 if compact else 240)
        if compact:
            self.device.setSizeAdjustPolicy(QComboBox.SizeAdjustPolicy.AdjustToMinimumContentsLengthWithIcon)
            self.device.setMinimumContentsLength(10)
            self.device.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Fixed)
        self.baud = combo(['115200'] if family == 'chameleon' else ['9600', '115200', '57600', '38400', '19200', '230400'])
        self.address = QSpinBox()
        self.address.setRange(0, 254)
        self.status = text_label('Disconnected', 'color: #a7b7c3;')
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        if family == 'uhf':
            self.reader_profile = combo(['Piswords UHF2', 'M5Stack JRD-4035', 'YRM100'])
            self.reader_profile.setAccessibleName('UHF reader profile')
            layout.addWidget(self.reader_profile)
            self.reader_profile.currentTextChanged.connect(self.profile_changed)
        if compact:
            layout.addWidget(self.mode)
            layout.addWidget(self.device)
            layout.addWidget(button('Refresh / scan', self.refresh))
        else:
            layout.addWidget(row(self.mode, self.device, button('Refresh / scan', self.refresh)))
        if compact:
            layout.addWidget(row(text_label('Baud'), self.baud))
            layout.addWidget(row(button('Connect', self.connect, True), button('Disconnect', self.disconnect)))
        else:
            fields = row(text_label('Baud'), self.baud, button('Connect', self.connect, True), button('Disconnect', self.disconnect))
            if family == 'uhf':
                fields.layout().insertWidget(2, text_label('Reader address'))
                fields.layout().insertWidget(3, self.address)
            layout.addWidget(fields)
        layout.addWidget(self.status)
        self.mode.currentTextChanged.connect(lambda _: self.device.clear())
    def profile_changed(self, profile):
        module = profile != 'Piswords UHF2'
        self.address.setEnabled(not module)
        self.baud.setCurrentText('115200' if module else '9600')
        self.window.uhf_power.setRange(18 if module else 0, 26 if module else 33)
        self.window.uhf_word.setMaximum(65535 if module else 2147483647)
        self.window.tags.clear()
        self.window.uhf_tags.setRowCount(0)
        self.window.uhf_selected.clear()
        self.window.uhf_memory.clear()
        for control in self.window.uhf_buzzer_buttons:
            control.setEnabled(not module)

    def refresh(self):
        mode = self.mode.currentText()
        self.window.run(lambda: self.window.service.discover(mode, self.family), self.discovered, 'Finding readers…')

    def discovered(self, devices):
        self.device.clear()
        for value, title in devices:
            self.device.addItem(title, value)
        self.window.note(f'{len(devices)} reader(s) found. Select yours and connect.')

    def connect(self):
        mode, device, baud, address = self.mode.currentText(), self.device.currentData(), int(self.baud.currentText()), self.address.value()
        profile = self.reader_profile.currentText() if self.family == 'uhf' else 'Piswords UHF2'
        self.window.run(lambda: self.window.service.connect(self.family, mode, device, baud, address, profile), self.connected, 'Connecting reader…')

    def connected(self, label):
        self.status.setText(label)
        self.window.note(label)
        if self.family == 'uhf':
            self.window.tags.clear()
            self.window.uhf_tags.setRowCount(0)
            self.window.uhf_selected.clear()
            self.window.uhf_memory.clear()
            self.reader_profile.setEnabled(False)
            self.mode.setEnabled(False)
            self.device.setEnabled(False)
            self.baud.setEnabled(False)
            self.address.setEnabled(False)
        if self.family == 'chameleon':
            if self.window.hf_readers.findText('Chameleon Ultra · connected') < 0:
                self.window.hf_readers.addItem('Chameleon Ultra · connected')

    def disconnect(self):
        def disconnected(_):
            self.status.setText('Disconnected')
            if self.family == 'uhf':
                self.reader_profile.setEnabled(True)
                self.mode.setEnabled(True)
                self.device.setEnabled(True)
                self.baud.setEnabled(True)
                self.address.setEnabled(self.reader_profile.currentText() == 'Piswords UHF2')
        self.window.run(lambda: self.window.service.disconnect(self.family), disconnected, 'Disconnecting…')


class RecordDialog(QDialog):
    def __init__(self, parent):
        super().__init__(parent)
        self.setWindowTitle('Add NFC record')
        self.setMinimumWidth(520)
        layout = QVBoxLayout(self)
        self.kind = combo(list(ndef.FIELDS))
        layout.addWidget(self.kind)
        self.form_widget = QWidget()
        self.form = QFormLayout(self.form_widget)
        layout.addWidget(self.form_widget)
        self.hint = text_label('Records are stored on the tag. The receiving phone decides how to use them.')
        layout.addWidget(self.hint)
        self.error = text_label('', 'color: #ffa99b;')
        layout.addWidget(self.error)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(self.save)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)
        self.kind.currentTextChanged.connect(self.populate)
        self.populate(self.kind.currentText())

    def populate(self, kind):
        while self.form.rowCount():
            self.form.removeRow(0)
        self.inputs = {}
        for key, label, default, choices in ndef.FIELDS[kind]:
            widget = combo(choices) if choices else edit(default, key == 'password')
            if choices:
                widget.setCurrentText(default)
            self.inputs[key] = widget
            self.form.addRow(label, widget)
        self.error.clear()

    def save(self):
        try:
            values = {k: w.currentText() if isinstance(w, QComboBox) else w.text() for k, w in self.inputs.items()}
            self.record = ndef.build(self.kind.currentText(), values)
            self.accept()
        except Exception as exc:
            self.error.setText(str(exc))


class MainWindow(QMainWindow):
    def __init__(self, settings=None):
        super().__init__()
        self.settings = settings if settings is not None else QSettings('Midwest Gadgets', 'Quadband Card Studio')
        self.setWindowTitle('Quadband Card Studio by Midwest Gadgets')
        self.resize(1250, 900)
        self.setMinimumSize(960, 680)
        if (ASSETS / 'MidwestGadgets.png').exists():
            self.setWindowIcon(QIcon(str(ASSETS / 'MidwestGadgets.png')))
        self.worker = Worker()
        self.service = self.worker.service
        self.worker.progress.connect(self.note)
        self.worker.completed.connect(self.done)
        self.worker.failed.connect(self.failed)
        self.worker.tag.connect(self.tag_received)
        self.worker.batch.connect(self.batch_result)
        self.worker.start()
        self.worker.ready.wait(2)
        self.busy, self.stop_flag, self.callback, self.closing, self.closed_ready = False, None, None, False, False
        self.snapshot = self.ndef_area = self.profile = self.lf_card = self.reader_snapshot = None
        self.records, self.tags, self.settings_inputs = [], {}, {}
        self.batch_success = self.batch_failure = 0
        self.build_ui()
        self.apply_theme()
        QApplication.instance().styleHints().colorSchemeChanged.connect(self.system_appearance_changed)

    def build_ui(self):
        central = QWidget()
        self.setCentralWidget(central)
        main = QHBoxLayout(central)
        sidebar = QWidget()
        self.sidebar = sidebar
        side = QVBoxLayout(sidebar)
        self.logo = BrandLogo()
        side.addWidget(self.logo, alignment=Qt.AlignmentFlag.AlignHCenter)
        side.addWidget(text_label('QUADBAND\nCard Studio', 'font-size: 25px; font-weight: 700; color: #edf6f2;'))
        side.addWidget(text_label('by Midwest Gadgets', 'color: #a7b7c3;'))
        self.appearance = combo(['System', 'Light', 'Dark'])
        self.appearance.setAccessibleName('Appearance')
        self.appearance.setCurrentText(self.settings.value('appearance', 'System'))
        self.appearance.currentTextChanged.connect(self.change_appearance)
        side.addWidget(row(text_label('Appearance'), self.appearance))
        side.addSpacing(22)
        self.nav = QListWidget()
        self.nav.addItems(['Magstripe Write', 'Magstripe Read', 'UHF Workspace', 'HF Workspace', 'LF Workspace', 'About & help'])
        self.nav.setFixedHeight(300)
        side.addWidget(self.nav)
        self.connection_panel = QWidget()
        connections = QVBoxLayout(self.connection_panel)
        connections.setContentsMargins(0, 0, 0, 0)
        self.msr_connection = Connection(self, 'msr', compact=True)
        self.lf_connection = Connection(self, 'chameleon', compact=True)
        connections.addWidget(group('Magstripe reader', self.msr_connection))
        connections.addWidget(group('Chameleon Ultra · HF / LF', self.lf_connection))
        side.addWidget(self.connection_panel)
        self.clear_button = button('Clear session', self.clear_session)
        side.addWidget(self.clear_button)
        side.addWidget(text_label(f'GPLv3 · Version {__version__}\nLocal data · No cloud account', 'color: #91a9b7; font-size: 12px;'))
        sidebar_scroll = QScrollArea()
        sidebar_scroll.setWidgetResizable(True)
        sidebar_scroll.setFixedWidth(280)
        sidebar_scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        sidebar_scroll.setWidget(sidebar)
        main.addWidget(sidebar_scroll)
        right = QVBoxLayout()
        main.addLayout(right, 1)
        self.stack = QStackedWidget()
        right.addWidget(self.stack, 1)
        for title, subtitle, build in [
            ('Magstripe Write', 'MSR X6 USB / Bluetooth · MSR605 serial · MSR605X HID', self.build_msr_write),
            ('Magstripe Read', 'MSR600 keyboard swipes and connected MSR writers', self.build_msr_read),
            ('UHF Workspace', 'Piswords UHF2 · M5Stack JRD-4035 · YRM100', self.build_uhf),
            ('HF Workspace', 'NFC records and Classic 1K data · ACS or Chameleon Ultra', self.build_hf),
            ('LF Workspace', 'Chameleon Ultra · LF identities and T5577 tools', self.build_lf),
            ('About & help', 'Open source card tools by Midwest Gadgets', self.build_about)]:
            page = QWidget()
            layout = QVBoxLayout(page)
            layout.setContentsMargins(24, 16, 24, 16)
            layout.setSpacing(16)
            layout.addWidget(text_label(title, 'font-size: 28px; font-weight: 700;'))
            layout.addWidget(text_label(subtitle, 'color: #a7b7c3;'))
            build(layout)
            layout.addStretch()
            scroll = QScrollArea()
            scroll.setWidgetResizable(True)
            scroll.setWidget(page)
            self.stack.addWidget(scroll)
        self.status = text_label('Ready. Choose a workspace and connect a reader.')
        self.status.setMinimumHeight(48)
        self.stop_button = button('Stop', self.stop)
        self.stop_button.setEnabled(False)
        right.addWidget(row(self.status, self.stop_button))
        self.nav.currentRowChanged.connect(self.stack.setCurrentIndex)
        self.nav.setCurrentRow(0)

    def build_msr_write(self, layout):
        layout.addWidget(text_label('Connect your magstripe reader by USB or Bluetooth in the sidebar.'))
        self.tracks = [edit() for _ in range(3)]
        self.densities = [combo(['210', '75']), combo(['75', '210']), combo(['210', '75'])]
        self.high_coercivity = QCheckBox('HiCo card')
        self.high_coercivity.setChecked(True)
        layout.addWidget(form_group('Track data', [(f'Track {i+1}', row(self.tracks[i], self.densities[i], text_label('BPI'))) for i in range(3)]))
        layout.addWidget(row(self.high_coercivity, button('Midwest sample', lambda: self.msr_sample(False)), button('Short 75-BPI test', lambda: self.msr_sample(True))))
        layout.addWidget(text_label('Standard ISO density: 210 / 75 / 210 BPI. HiCo describes the card coating. Writing may replace all tracks.'))
        layout.addWidget(row(button('Write & verify', self.msr_write, True), button('Verify existing card', self.msr_verify)))

    def msr_sample(self, short):
        for i, value in enumerate(magstripe.SHORT if short else magstripe.SAMPLE):
            self.tracks[i].setText(value)
            self.densities[i].setCurrentText('75' if short or i == 1 else '210')

    def msr_write(self):
        try:
            densities = [int(c.currentText()) for c in self.densities]
            tracks = magstripe.validate([e.text() for e in self.tracks], densities)
            high = self.high_coercivity.isChecked()
            if not self.review('Write magstripe card', f'Write {"HiCo" if high else "LoCo"} at {densities} BPI?\n\nAll tracks may be replaced. Swipe once to write, then swipe the same card again to verify.'):
                return
            self.run(lambda: self.service.require('msr').write_verify(tracks, densities, high, self.worker.progress.emit))
        except Exception as exc:
            self.failed(str(exc))

    def msr_verify(self):
        expected = [magstripe.normalize(e.text(), i) for i, e in enumerate(self.tracks)]
        async def job():
            require(any(expected), 'Enter the expected track data first.')
            actual = await self.service.require('msr').read()
            require(all(not e or e == actual[i] for i, e in enumerate(expected)), 'The card differs from the requested tracks.')
            return 'Requested tracks match. Empty tracks were not certified.'
        self.run(job, status='Swipe the card within 45 seconds…')

    def build_msr_read(self, layout):
        self.swipe_raw = editor(height=120)
        self.swipe_raw.setPlaceholderText('Click here and swipe an MSR600, or paste its raw swipe. Input stays inside this app.')
        self.swipe_raw.setTabChangesFocus(False)
        self.swipe_mapping = combo(magstripe.MAPPINGS)
        self.swipe_result = editor(True)
        self.swipe_timer = QTimer(self)
        self.swipe_timer.setSingleShot(True)
        self.swipe_timer.setInterval(450)
        self.swipe_timer.timeout.connect(self.parse_swipe)
        self.swipe_raw.textChanged.connect(lambda: self.swipe_timer.start() if not self.busy else None)
        self.swipe_mapping.currentTextChanged.connect(self.parse_swipe)
        layout.addWidget(group('MSR600 keyboard reader', self.swipe_raw, row(self.swipe_mapping, button('Interpret swipe', self.parse_swipe)), self.swipe_result))
        layout.addWidget(button('Read connected MSR X6 / MSR605', lambda: self.run(lambda: self.service.require('msr').read(), self.show_tracks, 'Swipe within 45 seconds…'), True))
        layout.addWidget(text_label('Connect your writer in Magstripe Write first. For an MSR600 with markers disabled, choose the matching field order. Unseparated fields cannot be split reliably.'))

    def parse_swipe(self, *_):
        if not self.swipe_raw.toPlainText():
            return
        try:
            self.show_tracks(magstripe.parse_swipe(self.swipe_raw.toPlainText(), self.swipe_mapping.currentData()))
        except Exception as exc:
            self.swipe_result.setPlainText(str(exc))

    def show_tracks(self, tracks):
        self.swipe_result.setPlainText('\n\n'.join(f'Track {i+1}\n{v or "(empty)"}' for i, v in enumerate(tracks)))
        self.note('Swipe captured. Results are held in this session.')

    def build_hf(self, layout):
        self.hf_readers = QComboBox()
        self.hf_readers.setMinimumWidth(300)
        self.hf_key, self.hf_key_type = edit('FFFFFFFFFFFF', True), combo({'Key A': 0x60, 'Key B': 0x61})
        layout.addWidget(group('HF reader', row(self.hf_readers, button('Refresh readers', self.refresh_hf), button('Read card', self.read_hf, True)),
                               row(text_label('Additional Classic key'), self.hf_key, self.hf_key_type),
                               text_label('Standard MAD, NDEF and factory keys are tried automatically. Chameleon connects in LF Workspace.')))
        self.hf_readers.currentTextChanged.connect(self.hf_changed)
        tabs = QTabWidget()
        layout.addWidget(tabs)
        read = QWidget()
        rl = QVBoxLayout(read)
        self.hf_info = editor(True, 240)
        rl.addWidget(self.hf_info)
        rl.addWidget(row(button('Save credentials', self.save_profile), button('Use scan as copy source', self.use_profile)))
        tabs.addTab(read, 'Read')
        write = QWidget()
        wl = QVBoxLayout(write)
        self.record_list = QListWidget()
        self.record_list.setMinimumHeight(200)
        wl.addWidget(self.record_list)
        wl.addWidget(row(button('Add record', self.add_record), button('Remove', self.remove_record), button('Move up', lambda: self.move_record(-1)), button('Move down', lambda: self.move_record(1)), button('Use existing', self.use_existing_records)))
        self.record_size = text_label('0 records')
        wl.addWidget(self.record_size)
        wl.addWidget(button('Write records & verify', self.write_records, True))
        wl.addWidget(text_label('Read the target first. Writes replace its record message and require an already NFC-formatted Classic 1K card.'))
        tabs.addTab(write, 'Write records')
        copy = QWidget()
        cl = QVBoxLayout(copy)
        self.profile_label = text_label('Scan card or upload card credentials to choose a source.')
        cl.addWidget(self.profile_label)
        cl.addWidget(row(button('Scan source card', lambda: self.read_hf(for_copy=True)), button('Upload credentials', self.load_profile), button('Write from file / source', self.write_profile)))
        cl.addWidget(button('Copy to infinity', self.start_batch, True))
        self.batch_totals = text_label('0 successes · 0 failures')
        cl.addWidget(self.batch_totals)
        self.batch_log = editor(True, 180)
        self.batch_log.setMaximumBlockCount(500)
        cl.addWidget(self.batch_log)
        cl.addWidget(text_label('Copies 47 data blocks while preserving each target’s UID and keys. Failures are listed and the batch continues. Remove every card before the next copy. Stop finishes the current card first.'))
        tabs.addTab(copy, 'Copy && files')
        advanced = QWidget()
        al = QVBoxLayout(advanced)
        self.block_table = QTableWidget(0, 3)
        self.block_table.setHorizontalHeaderLabels(['Block', 'Hex data', 'State'])
        self.block_table.horizontalHeader().setSectionResizeMode(1, QHeaderView.ResizeMode.Stretch)
        self.block_table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self.block_table.setMinimumHeight(240)
        al.addWidget(self.block_table)
        self.block_number = QSpinBox()
        self.block_number.setRange(0, 63)
        self.block_data = edit()
        al.addWidget(row(self.block_number, self.block_data, button('Write block & verify', self.write_hf_block)))
        self.block_table.cellClicked.connect(self.select_block)
        tabs.addTab(advanced, 'Advanced')
        tools = QWidget()
        tl = QVBoxLayout(tools)
        tl.addWidget(row(button('Read reader settings', self.read_reader_settings), button('Buzzer off', lambda: self.reader_preset('mute')), button('Buzzer on', lambda: self.reader_preset('unmute'))))
        tl.addWidget(row(button('Tototo31 · 80% / 250 ms', lambda: self.reader_preset('tototo31')), button('Ashsh · 100% / continuous', lambda: self.reader_preset('ashsh')), button('RF defaults', lambda: self.reader_preset('rfDefaults'))))
        self.settings_form = QFormLayout()
        tl.addLayout(self.settings_form)
        tl.addWidget(button('Review & apply settings', self.apply_reader_settings, True))
        tl.addWidget(text_label('ACR122U and ACR1552U controls use the documented ACS commands. Power changes restart the 1552; saved polling/UI changes require a replug. Percentage settings may have no effect on some hardware. RF presets credit Tototo31 and Ashsh.'))
        tabs.addTab(tools, 'Reader tools')

    def hf_changed(self, *_):
        self.snapshot = self.ndef_area = self.reader_snapshot = None
        if hasattr(self, 'hf_info'):
            self.hf_info.clear()
            self.block_table.setRowCount(0)
        if hasattr(self, 'settings_form'):
            while self.settings_form.rowCount():
                self.settings_form.removeRow(0)
            self.settings_inputs = {}

    def key(self):
        return hex_bytes(self.hf_key.text(), 6), self.hf_key_type.currentData()

    def refresh_hf(self):
        def found(names):
            selected = self.hf_readers.currentText()
            self.hf_readers.clear()
            self.hf_readers.addItems(names)
            if self.service.chameleon and not self.service.chameleon.transport.closed:
                self.hf_readers.addItem('Chameleon Ultra · connected')
            if self.hf_readers.findText(selected) >= 0:
                self.hf_readers.setCurrentText(selected)
            self.note('Select a reader, place a card, and read it.')
        self.run(lambda: call_sync(pcsc.readers), found, 'Finding HF readers…')

    def read_hf(self, checked=False, for_copy=False):
        try:
            name, key = self.hf_readers.currentText(), self.key()
            def complete(snapshot):
                self.show_snapshot(snapshot)
                if for_copy:
                    self.use_profile()
            self.run(lambda: self.service.hf_read(name, key), complete, 'Reading card and NFC records…')
        except Exception as exc:
            self.failed(str(exc))

    def show_snapshot(self, snapshot):
        self.snapshot, self.ndef_area = snapshot, None
        info = snapshot.card
        lines = ['MIFARE Classic 1K' if info.classic else 'Other NFC card (identity only)',
                 'UID: '+info.uid.hex(':').upper()]
        if info.atr:
            lines.append('ATR: '+info.atr.hex(' ').upper())
        if info.atqa:
            lines.append(f'ATQA: {info.atqa.hex().upper()} · SAK: {info.sak:02X}')
        lines.append(f'Read {len(snapshot.blocks)} blocks; {len(snapshot.errors)} unreadable.')
        if info.classic:
            try:
                self.ndef_area = hf.ClassicNDEF(snapshot)
                lines += [f'NFC records: {self.ndef_area.size} / {self.ndef_area.capacity} bytes',
                          'Writable: '+('Yes' if self.ndef_area.writable else 'No, with the available keys')]
                for i, record in enumerate(self.ndef_area.records):
                    title, value = record.display()
                    lines += [f'\nRecord {i+1} · {title}', value]
            except CardError as exc:
                lines.append(str(exc))
        self.hf_info.setPlainText('\n'.join(lines))
        self.block_table.setRowCount(64 if info.classic else 0)
        for b in range(self.block_table.rowCount()):
            state = snapshot.errors.get(b, 'Data' if hf.data_block(b) else 'Protected')
            for col, value in enumerate([str(b), snapshot.blocks.get(b, b'').hex(' ').upper(), state]):
                self.block_table.setItem(b, col, QTableWidgetItem(value))
        self.note('Read complete.' if not snapshot.errors else 'Some blocks could not be read. Check the key and card placement.')

    def add_record(self):
        dialog = RecordDialog(self)
        if dialog.exec() == QDialog.DialogCode.Accepted:
            self.records.append(dialog.record)
            self.update_records()

    def update_records(self):
        self.record_list.clear()
        for r in self.records:
            title, value = r.display()
            self.record_list.addItem(title+' · '+value.replace('\n', ' ')[:160])
        try:
            self.record_size.setText(f'{len(self.records)} records · {len(ndef.encode(self.records)) if self.records else 0} bytes')
        except CardError as exc:
            self.record_size.setText(str(exc))

    def remove_record(self):
        i = self.record_list.currentRow()
        if 0 <= i < len(self.records):
            self.records.pop(i)
            self.update_records()

    def move_record(self, direction):
        i = self.record_list.currentRow()
        if 0 <= i < len(self.records) and 0 <= i+direction < len(self.records):
            self.records[i], self.records[i+direction] = self.records[i+direction], self.records[i]
            self.update_records()
            self.record_list.setCurrentRow(i+direction)

    def use_existing_records(self):
        if self.ndef_area:
            self.records = list(self.ndef_area.records)
            self.update_records()
        else:
            self.note('Read an NFC-formatted target first.')

    def write_records(self):
        try:
            require(self.ndef_area is not None, 'Read the target’s NFC records first.')
            records, expected, key, name = list(self.records), self.ndef_area, self.key(), self.hf_readers.currentText()
            expected.replacing(records)
            if not self.review('Write NFC records', f'Replace the records on card {expected.card.uid.hex().upper()} with {len(records)} queued record(s)?\nUID, keys and sector trailers remain unchanged.'):
                return
            async def job():
                async with self.service.hf_session(name) as client:
                    return await client.write_records(records, expected, key)
            self.run(job, self.show_snapshot, 'Writing records and verifying…')
        except Exception as exc:
            self.failed(str(exc))

    def use_profile(self):
        try:
            require(self.snapshot is not None, 'Scan the source card first.')
            self.profile = hf.profile_from_snapshot(self.snapshot)
            self.profile_label.setText('Source ready · 47 data blocks · UID and keys excluded')
            self.note('Copy source captured. Remove it before placing target cards.')
        except Exception as exc:
            self.failed(str(exc))

    def save_profile(self):
        try:
            require(self.snapshot is not None, 'Read all source sectors first.')
            profile = hf.profile_from_snapshot(self.snapshot)
            path, _ = QFileDialog.getSaveFileName(self, 'Save card credentials', 'Classic-1K.qcard', 'Quadband card (*.qcard)')
            if path:
                save_json(path, profile)
                self.note('Credentials saved locally. UID and authentication keys are excluded.')
        except Exception as exc:
            self.failed(str(exc))

    def load_profile(self):
        path, _ = QFileDialog.getOpenFileName(self, 'Upload card credentials', '', 'Quadband card (*.qcard)')
        if path:
            try:
                profile = load_json(path)
                hf.profile_blocks(profile)
                self.profile = profile
                self.profile_label.setText(f'{Path(path).name} · 47 data blocks ready')
            except Exception as exc:
                self.failed(str(exc))

    def write_profile(self):
        try:
            require(self.profile is not None, 'Scan a source or upload a .qcard file first.')
            profile, name, key = self.profile, self.hf_readers.currentText(), self.key()
            hf.profile_blocks(profile)
            if not self.review('Write card data', 'Place one target card. Replace its 47 data blocks with the loaded source?\nEach card keeps its UID and keys. This is a data copy, not NFC formatting.'):
                return
            async def job():
                async with self.service.hf_session(name) as client:
                    await client.write_profile(profile, key)
                return 'All 47 target data blocks verified.'
            self.snapshot = self.ndef_area = None
            self.run(job, status='Copying and verifying data…')
        except Exception as exc:
            self.failed(str(exc))

    def start_batch(self):
        try:
            require(self.profile is not None, 'Scan card or upload card credentials first.')
            profile, name, key = self.profile, self.hf_readers.currentText(), self.key()
            hf.profile_blocks(profile)
            if not self.review('Copy to infinity', 'Start copying the source to every target presented?\nRemove the source first. Each target keeps its UID and keys. Failures will be listed and the batch will continue.'):
                return
            self.stop_flag = StopFlag()
            stop = self.stop_flag
            self.snapshot = self.ndef_area = None
            self.run(lambda: hf.copy_forever(lambda: self.service.hf_session(name), profile, key, stop,
                                             self.worker.progress.emit, self.worker.batch.emit), status='Remove the source card to begin.', graceful=True)
        except Exception as exc:
            self.failed(str(exc))

    def batch_result(self, success, detail):
        self.batch_success += int(success)
        self.batch_failure += int(not success)
        self.batch_totals.setText(f'{self.batch_success} successes · {self.batch_failure} failures')
        self.batch_log.appendPlainText(datetime.now().strftime('%H:%M:%S')+' · '+('SUCCESS' if success else 'FAILED')+' · '+detail.replace('\n', ' '))

    def select_block(self, block, _):
        self.block_number.setValue(block)
        if self.snapshot:
            self.block_data.setText(self.snapshot.blocks.get(block, b'').hex(' ').upper())

    def write_hf_block(self):
        try:
            snapshot, block, data, name = self.snapshot, self.block_number.value(), hex_bytes(self.block_data.text(), 16), self.hf_readers.currentText()
            require(snapshot is not None and hf.data_block(block), 'Read a data block first. UID and sector trailers are protected.')
            if not self.review('Write data block', f'Replace block {block} on card {snapshot.card.uid.hex().upper()}?\n{data.hex(" ").upper()}'):
                return
            async def job():
                async with self.service.hf_session(name) as client:
                    await client.write_block(block, data, snapshot)
                return f'Block {block} verified. Read the card again to refresh the snapshot.'
            self.snapshot = self.ndef_area = None
            self.run(job)
        except Exception as exc:
            self.failed(str(exc))

    def read_reader_settings(self):
        name = self.hf_readers.currentText()
        self.run(lambda: reader_tools.ACS(pcsc.ReaderControl(name).control, name).read(), self.show_reader_settings, 'Reading reader settings…')

    def show_reader_settings(self, snapshot):
        self.reader_snapshot = snapshot
        while self.settings_form.rowCount():
            self.settings_form.removeRow(0)
        self.settings_inputs = {}
        self.settings_form.addRow(text_label(snapshot['firmware']))
        for field in reader_tools.fields(snapshot['family']):
            widget = QComboBox()
            widget.addItem('Keep current setting', None)
            for value, title in field.choices.items():
                widget.addItem(title, value)
            current = field.value(snapshot)
            widget.setToolTip('Current: '+field.choices.get(current, 'Not reported'))
            enabled = reader_tools.supports(snapshot, field.feature) and (field.register in reader_tools.WRITE_ONLY or field.register in snapshot['values'])
            widget.setEnabled(enabled)
            self.settings_inputs[field.id] = widget
            self.settings_form.addRow(field.title, widget)
        self.note('Reader settings loaded. Stage changes, then review and apply.')

    def reader_preset(self, name):
        try:
            require(self.reader_snapshot is not None, 'Read reader settings first.')
            edits = reader_tools.preset(self.reader_snapshot, name)
            for id, value in edits.items():
                widget = self.settings_inputs[id]
                widget.setCurrentIndex(widget.findData(value))
            self.note('Preset staged. Review & apply when ready.')
        except Exception as exc:
            self.failed(str(exc))

    def apply_reader_settings(self):
        try:
            snapshot = self.reader_snapshot
            name = self.hf_readers.currentText()
            require(snapshot is not None and snapshot['reader'] == name, 'Read settings from the selected reader first.')
            edits = {id: w.currentData() for id, w in self.settings_inputs.items() if w.currentData() is not None}
            changes = reader_tools.plan(snapshot, edits)
            require(bool(changes), 'Choose a setting to change.')
            detail = '\n'.join(title for _, _, title in changes)
            if edits.get('host') == 0:
                detail += '\n\nKeyboard-only mode removes PC/SC. A separate ACS utility is needed to restore card-reader mode.'
            if not self.review('Apply reader settings', detail):
                return
            self.snapshot = self.ndef_area = self.reader_snapshot = None
            self.run(lambda: reader_tools.ACS(pcsc.ReaderControl(name).control, name).apply(snapshot, edits), status='Applying reader settings…')
        except Exception as exc:
            self.failed(str(exc))

    def build_uhf(self, layout):
        self.uhf_connection = Connection(self, 'uhf')
        layout.addWidget(group('UHF reader · USB serial', self.uhf_connection))
        layout.addWidget(text_label('M5Stack JRD-4035 / YRM100: use a USB-TTL serial adapter (normally 115200, 8N1), suitable module power and matching logic voltage. Module profiles have no reader address or buzzer control. Exact memory selection supports EPCs up to 28 bytes; writes support up to 32 words.'))
        layout.addWidget(row(button('Scan tags', self.scan_uhf, True), button('Save scan CSV', self.save_uhf)))
        self.uhf_tags = QTableWidget(0, 5)
        self.uhf_tags.setHorizontalHeaderLabels(['EPC', 'Antenna', 'Frequency kHz', 'RSSI dBm', 'Reads'])
        self.uhf_tags.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        self.uhf_tags.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self.uhf_tags.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        self.uhf_tags.setMinimumHeight(180)
        self.uhf_tags.cellClicked.connect(self.select_uhf)
        layout.addWidget(self.uhf_tags)
        self.uhf_selected = edit()
        self.uhf_selected.setReadOnly(True)
        self.uhf_bank = combo({'Reserved / passwords': 0, 'EPC': 1, 'TID / chip ID': 2, 'User memory': 3})
        self.uhf_bank.setCurrentIndex(2)
        self.uhf_word = QSpinBox()
        self.uhf_word.setRange(0, 2147483647)
        self.uhf_count = QSpinBox()
        self.uhf_count.setRange(1, 64)
        self.uhf_count.setValue(6)
        self.uhf_password = edit('00000000', True)
        self.uhf_memory = editor(True)
        self.uhf_data, self.uhf_new_epc = edit(), edit()
        layout.addWidget(form_group('Tag memory', [('Selected EPC', self.uhf_selected), ('Memory bank', self.uhf_bank),
            ('Starting word / count', row(self.uhf_word, self.uhf_count)), ('Access password', self.uhf_password),
            ('Read result', self.uhf_memory), ('User-memory hex', self.uhf_data), ('New EPC hex', self.uhf_new_epc)]))
        layout.addWidget(row(button('Read memory', self.read_uhf), button('Write User & verify', self.write_uhf), button('Change EPC & verify', self.change_epc, True)))
        self.uhf_power = QSpinBox()
        self.uhf_power.setRange(0, 33)
        self.uhf_power.setValue(20)
        self.uhf_buzzer_buttons = [button('Buzzer off', lambda: self.run(lambda: self.service.require('uhf').buzzer(False))),
                                  button('Buzzer on', lambda: self.run(lambda: self.service.require('uhf').buzzer(True)))]
        layout.addWidget(group('Reader tools', row(*self.uhf_buzzer_buttons, self.uhf_power,
            button('Set RF power (dBm)', self.set_uhf_power))))
        layout.addWidget(text_label('Keep one intended tag near the reader. EPC selection cannot distinguish tags sharing an EPC. Stop inventory before accessing memory.'))

    def scan_uhf(self):
        self.stop_flag = StopFlag()
        stop = self.stop_flag
        async def job():
            client = self.service.require('uhf')
            client.on_tag = self.worker.tag.emit
            return await client.inventory(stop)
        self.run(job, status='Scanning UHF tags. Click Stop to access memory.', graceful=True)

    def tag_received(self, tag):
        epc = tag['epc'].hex().upper()
        if epc not in self.tags and len(self.tags) >= 2000:
            return
        tag['count'] = self.tags.get(epc, {}).get('count', 0)+1
        self.tags[epc] = tag
        keys = list(self.tags)
        i = keys.index(epc)
        if self.uhf_tags.rowCount() <= i:
            self.uhf_tags.setRowCount(i+1)
        for col, value in enumerate([epc, tag['antenna'], tag['frequency'] if tag['frequency'] is not None else 'Not reported',
                                     tag.get('signal', 'Not reported'), tag['count']]):
            self.uhf_tags.setItem(i, col, QTableWidgetItem(str(value)))
        if not self.uhf_selected.text():
            self.uhf_selected.setText(epc)
            self.uhf_new_epc.setText(epc)

    def select_uhf(self, index, _):
        item = self.uhf_tags.item(index, 0)
        if item:
            self.uhf_selected.setText(item.text())
            self.uhf_new_epc.setText(item.text())
            self.uhf_memory.clear()

    def uhf_args(self):
        epc = hex_bytes(self.uhf_selected.text())
        require(bool(epc), 'Scan and select a UHF tag first.')
        return epc, self.uhf_bank.currentData(), self.uhf_word.value(), self.uhf_count.value(), hex_bytes(self.uhf_password.text(), 4)

    def read_uhf(self):
        try:
            args = self.uhf_args()
            self.run(lambda: self.service.require('uhf').read(*args), lambda data: self.uhf_memory.setPlainText(data.hex(' ').upper()), 'Reading UHF memory…')
        except Exception as exc:
            self.failed(str(exc))

    def write_uhf(self):
        try:
            epc, _, word, _, password = self.uhf_args()
            module = self.uhf_connection.reader_profile.currentText() != 'Piswords UHF2'
            data = hex_bytes(self.uhf_data.text(), maximum=64 if module else 128)
            require(bool(data) and len(data) % 2 == 0, 'Enter complete User-memory words.')
            if not self.review('Write UHF User memory', f'Write {len(data)//2} words to User memory starting at word {word}?\nEPC: {epc.hex().upper()}\n{data.hex(" ").upper()}'):
                return
            self.run(lambda: self.service.require('uhf').write(epc, 3, word, data, password),
                     lambda _: self.note('User memory verified.'), 'Writing UHF User memory…')
        except Exception as exc:
            self.failed(str(exc))

    def change_epc(self):
        try:
            old, _, _, _, password = self.uhf_args()
            module = self.uhf_connection.reader_profile.currentText() != 'Piswords UHF2'
            new = hex_bytes(self.uhf_new_epc.text(), maximum=28 if module else 62)
            if not self.review('Change UHF EPC', f'Current: {old.hex().upper()}\nNew: {new.hex().upper()}\n\nKeep only the intended tag nearby. The app will preserve the other control bits and verify the new EPC.'):
                return
            def changed(message):
                self.uhf_selected.setText(new.hex().upper())
                self.uhf_memory.clear()
                self.tags.clear()
                self.uhf_tags.setRowCount(0)
                self.note(message+' Scan again to refresh inventory.')
            self.run(lambda: self.service.require('uhf').change_epc(old, new, password), changed, 'Changing EPC and verifying…')
        except Exception as exc:
            self.failed(str(exc))

    def set_uhf_power(self):
        value = self.uhf_power.value()
        if self.review('Set UHF RF power', f'Set the reader to {value} dBm and check read-back?'):
            self.run(lambda: self.service.require('uhf').power(value))

    def save_uhf(self):
        path, _ = QFileDialog.getSaveFileName(self, 'Save UHF inventory', 'UHF-inventory.csv', 'CSV (*.csv)')
        if path:
            try:
                with open(path, 'w', newline='', encoding='utf-8') as stream:
                    writer = csv.writer(stream)
                    writer.writerow(['EPC', 'Antenna', 'Frequency kHz', 'Reads'])
                    writer.writerows([epc, t['antenna'], t['frequency'], t['count']] for epc, t in self.tags.items())
                self.note('Inventory saved locally.')
            except Exception as exc:
                self.failed(str(exc))

    def build_lf(self, layout):
        layout.addWidget(text_label('Connect Chameleon Ultra by USB or Bluetooth in the sidebar. The same connection serves HF and LF.'))
        self.lf_format = combo(['Automatic']+list(chameleon.FORMATS))
        self.lf_hint = QSpinBox()
        self.lf_hint.setRange(0, 42)
        self.lf_hint.setToolTip('0 = automatic HID format. Use a known firmware format code when ambiguous.')
        layout.addWidget(row(self.lf_format, text_label('HID format hint'), self.lf_hint, button('Scan & read LF', self.scan_lf, True)))
        self.lf_result = editor(True)
        layout.addWidget(self.lf_result)
        layout.addWidget(row(button('Save LF card', self.save_lf), button('Open LF card', self.load_lf), button('Clone to T5577', lambda: self.write_lf('clone'), True)))
        self.lf_default = edit('0102030405')
        layout.addWidget(form_group('T5577 tools · no passwords', [('Default EM410x test ID', self.lf_default)]))
        layout.addWidget(row(button('Write default value', lambda: self.write_lf('default')), button('Recover T5577', lambda: self.write_lf('recover'))))
        layout.addWidget(text_label('Password writes are disabled. Writing requires Chameleon firmware with open block-write support. Password-free encoders support EM410x, Electra, Viking, ioProx, PAC and Jablotron; HID/Pyramid are read-only. Recovery cannot repair damaged, locked or password-protected cards.'))
        layout.addWidget(text_label('HF cards: connect the Chameleon in the sidebar, then select it in HF Workspace. LF scans capture a transmitted identity, not a complete T5577 memory dump.'))

    def scan_lf(self):
        format, hint = self.lf_format.currentText(), self.lf_hint.value()
        self.run(lambda: self.service.require('chameleon').scan(None if format == 'Automatic' else format, hint), self.show_lf, 'Scanning the LF antenna…')

    def show_lf(self, card):
        self.lf_card = card
        if card is None:
            self.lf_result.setPlainText('No supported LF card found. Center one card on the LF antenna.')
        else:
            self.lf_result.setPlainText(f'{card.format}\nID: {card.data.hex().upper()}\n{len(card.data)} bytes · No authentication password is included in saved LF files.')
        self.note('LF scan complete.')

    def save_lf(self):
        try:
            require(self.lf_card is not None, 'Scan or open an LF card first.')
            path, _ = QFileDialog.getSaveFileName(self, 'Save LF identity', 'LF-card.qlf', 'Quadband LF (*.qlf)')
            if path:
                save_json(path, self.lf_card.as_file())
        except Exception as exc:
            self.failed(str(exc))

    def load_lf(self):
        path, _ = QFileDialog.getOpenFileName(self, 'Open LF identity', '', 'Quadband LF (*.qlf)')
        if path:
            try:
                self.show_lf(chameleon.LFCard.from_file(load_json(path, 16384)))
            except Exception as exc:
                self.failed(str(exc))

    def write_lf(self, mode):
        try:
            card = self.lf_card if mode == 'clone' else chameleon.LFCard('EM410x', hex_bytes(self.lf_default.text(), 5))
            require(card is not None, 'Scan or open the source LF card first.')
            title = {'clone': 'Clone LF identity', 'default': 'Write T5577 default', 'recover': 'Recover T5577'}[mode]
            if not self.review(title, f'Replace the target with {card.format}: {card.data.hex().upper()}?\n\nNo password will be written. Password protection stays disabled. Already protected cards are not supported.\n\nRemove the source and leave one T5577 target on the LF antenna. Recovery uses the same writer and cannot repair every unreadable card.'):
                return
            self.run(lambda: self.service.require('chameleon').write(card), status='Writing T5577 and verifying its identity…')
        except Exception as exc:
            self.failed(str(exc))

    def build_about(self, layout):
        layout.addWidget(group('Quadband Card Studio', text_label(f'Version {__version__} · Copyright © 2026 Midwest Gadgets'),
            text_label('Source code is licensed under GNU GPL version 3. The Midwest Gadgets name and logo are subject to separate branding terms. This software comes with no warranty. See LICENSE and BRANDING.txt in the source distribution.'),
            text_label('Windows 10/11: install the reader’s CCID or USB-serial driver if needed. Keep the Windows Smart Card service enabled. Bluetooth uses the Windows pairing flow.'),
            text_label('Linux: install pcsc-lite/pcscd, libccid, hidapi and BlueZ. Serial and HID access may need the supplied udev rules and dialout/uucp group membership. Run the app as your normal user.'),
            text_label('Use one application per reader. USB and Bluetooth variants differ by firmware. Physical writes are never automatically retried; read the card after any interrupted or unverified write.'),
            text_label('The port has automated protocol and simulated-reader tests. Windows/Linux hardware validation is still required. The native Mac app and all source files are included in the project.'),
            button('Show license', self.show_license)))
        layout.addWidget(group('Data and privacy', text_label('Card data stays local. Nothing is uploaded, and there is no telemetry. Files are saved only when you choose Save. .qcard files contain 47 HF data blocks; .qlf files contain an LF format and identity. Both work with the native Mac app. Clear session removes data from this window; it does not erase files or the system clipboard.')))

    def show_license(self):
        dialog = QDialog(self)
        dialog.setWindowTitle('GNU General Public License, version 3')
        dialog.resize(750, 650)
        layout = QVBoxLayout(dialog)
        content = editor(True)
        path = ASSETS / 'LICENSE'
        content.setPlainText(path.read_text(encoding='utf-8') if path.exists() else 'See LICENSE in the full source distribution.')
        layout.addWidget(content)
        layout.addWidget(button('Close', dialog.accept))
        dialog.exec()

    def review(self, title, message):
        box = QMessageBox(QMessageBox.Icon.Question, title, message,
                          QMessageBox.StandardButton.Cancel | QMessageBox.StandardButton.Ok, self)
        box.setTextFormat(Qt.TextFormat.PlainText)
        box.setDefaultButton(QMessageBox.StandardButton.Cancel)
        box.button(QMessageBox.StandardButton.Ok).setText('Write / apply')
        return box.exec() == QMessageBox.StandardButton.Ok

    def run(self, factory, callback=None, status='Working…', graceful=False):
        if self.busy:
            return
        self.busy, self.callback = True, callback
        if not graceful:
            self.stop_flag = None
        self.stack.setEnabled(False)
        self.connection_panel.setEnabled(False)
        self.clear_button.setEnabled(False)
        self.stop_button.setEnabled(True)
        self.note(status)
        self.worker.submit(factory)

    def note(self, message):
        if hasattr(self, 'status'):
            self.status.setText(str(message))
            self.status.setStyleSheet('')

    def finish(self):
        self.busy = False
        self.stack.setEnabled(True)
        self.connection_panel.setEnabled(True)
        self.clear_button.setEnabled(True)
        self.stop_button.setEnabled(False)
        self.stop_flag = None

    def done(self, result):
        callback, self.callback = self.callback, None
        self.finish()
        try:
            if callback:
                callback(result)
            elif isinstance(result, str):
                self.note(result)
            else:
                self.note('Operation complete.')
        except Exception as exc:
            self.failed(str(exc))
        if self.closing:
            self.begin_close()

    def failed(self, message):
        self.callback = None
        self.finish()
        self.status.setText(message)
        self.status.setStyleSheet(f'color: {"#ffa99b" if self.dark_mode else "#a52d20"};')
        if self.closing:
            self.begin_close()

    def stop(self):
        if self.stop_flag:
            self.stop_flag.set()
            self.note('Stopping after the current card / reader command…')
        elif self.worker.future:
            self.worker.future.cancel()
            self.note('Stopping…')
        self.stop_button.setEnabled(False)

    def clear_session(self):
        if self.busy:
            return
        self.snapshot = self.ndef_area = self.profile = self.lf_card = self.reader_snapshot = None
        self.records, self.tags = [], {}
        for widget in [*self.tracks, self.swipe_raw, self.swipe_result, self.hf_info, self.block_data,
                       self.uhf_selected, self.uhf_data, self.uhf_new_epc, self.uhf_memory,
                       self.lf_result, self.batch_log]:
            widget.clear()
        self.hf_key.setText('FFFFFFFFFFFF')
        self.uhf_password.setText('00000000')
        self.block_table.setRowCount(0)
        self.uhf_tags.setRowCount(0)
        self.batch_success = self.batch_failure = 0
        self.batch_totals.setText('0 successes · 0 failures')
        self.profile_label.setText('Scan card or upload card credentials to choose a source.')
        self.update_records()
        self.note('Session cleared.')

    def begin_close(self):
        self.closing = False
        def closed(_):
            self.closed_ready = True
            self.worker.loop.call_soon_threadsafe(self.worker.loop.stop)
            self.worker.wait(3000)
            self.close()
        self.run(self.service.close, closed, 'Closing readers…')

    def change_appearance(self, mode):
        self.settings.setValue('appearance', mode)
        self.apply_theme()

    def system_appearance_changed(self, scheme):
        if self.appearance.currentText() == 'System':
            self.apply_theme()

    def apply_theme(self):
        app = QApplication.instance()
        mode = self.appearance.currentText()
        self.dark_mode = mode == 'Dark' or (mode == 'System' and app.styleHints().colorScheme() == Qt.ColorScheme.Dark)
        colors = {'window': '#14202b', 'text': '#e4edf2', 'base': '#101922', 'button': '#293b49',
                  'muted': '#a7b7c3', 'selection': '#266a59'} if self.dark_mode else {
                  'window': '#f3f6f8', 'text': '#182b38', 'base': '#ffffff', 'button': '#e3eaf0',
                  'muted': '#526577', 'selection': '#246b55'}
        palette = QPalette()
        for role, color in [(QPalette.ColorRole.Window, 'window'), (QPalette.ColorRole.WindowText, 'text'),
                            (QPalette.ColorRole.Base, 'base'), (QPalette.ColorRole.AlternateBase, 'window'),
                            (QPalette.ColorRole.Text, 'text'), (QPalette.ColorRole.Button, 'button'),
                            (QPalette.ColorRole.ButtonText, 'text'), (QPalette.ColorRole.ToolTipBase, 'base'),
                            (QPalette.ColorRole.ToolTipText, 'text'), (QPalette.ColorRole.Highlight, 'selection')]:
            palette.setColor(role, QColor(colors[color]))
        palette.setColor(QPalette.ColorRole.HighlightedText, QColor('#ffffff'))
        for role in (QPalette.ColorRole.Text, QPalette.ColorRole.WindowText, QPalette.ColorRole.ButtonText):
            palette.setColor(QPalette.ColorGroup.Disabled, role, QColor(colors['muted']))
        app.setPalette(palette)
        style = STYLE
        if not self.dark_mode:
            replacements = {'#14202b': '#f3f6f8', '#e4edf2': '#182b38', '#334653': '#b8c7d1',
                            '#9edbc6': '#17634f', '#101922': '#ffffff', '#3a4c59': '#a7b9c6',
                            '#293b49': '#e3eaf0', '#435968': '#9cadbb', '#365063': '#d3e1e9',
                            '#78909c': '#647785', '#20313e': '#e3eaf0', '#243744': '#e3eaf0'}
            for old, new in replacements.items():
                style = style.replace(old, new)
        style += f'\nQLabel[tone="muted"] {{ color: {colors["muted"]}; }}'
        style += f'\nQLabel[tone="error"] {{ color: {"#ffa99b" if self.dark_mode else "#a52d20"}; }}'
        style += '\nQPushButton[primary="true"], QListWidget::item:selected, QTabBar::tab:selected { color: #ffffff; }'
        style += f'\nQToolTip {{ background: {colors["base"]}; color: {colors["text"]}; }}'
        app.setStyleSheet(style)
        self.status.setStyleSheet('')

    def closeEvent(self, event):
        if self.closed_ready:
            event.accept()
            return
        event.ignore()
        if self.busy:
            box = QMessageBox.question(self, 'Operation in progress', 'Stop the current operation and close? A command already sent cannot be undone.',
                                       QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No, QMessageBox.StandardButton.No)
            if box != QMessageBox.StandardButton.Yes:
                return
            self.closing = True
            self.stop()
        else:
            self.begin_close()


STYLE = '''
QWidget { background: #14202b; color: #e4edf2; font-family: "Segoe UI", "Noto Sans", sans-serif; font-size: 13px; }
QMainWindow, QScrollArea { background: #14202b; }
QGroupBox { border: 1px solid #334653; border-radius: 10px; margin-top: 12px; padding: 16px 12px 12px; font-weight: 600; }
QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 6px; color: #9edbc6; }
QLineEdit, QPlainTextEdit, QComboBox, QSpinBox, QListWidget, QTableWidget { background: #101922; border: 1px solid #3a4c59; border-radius: 5px; padding: 7px; selection-background-color: #266a59; }
QPushButton { background: #293b49; border: 1px solid #435968; border-radius: 6px; padding: 9px 13px; }
QPushButton:hover { background: #365063; }
QPushButton[primary="true"] { background: #246b55; border: 1px solid #46977c; font-weight: 600; }
QPushButton:disabled, QComboBox:disabled, QLineEdit:disabled { color: #78909c; }
QListWidget::item { padding: 11px 7px; }
QListWidget::item:selected { background: #245c4e; border-radius: 5px; }
QTabWidget::pane { border: 1px solid #334653; padding: 10px; }
QTabBar::tab { background: #20313e; padding: 10px 12px; }
QTabBar::tab:selected { background: #245c4e; }
QHeaderView::section { background: #243744; padding: 8px; border: none; }
QScrollBar:vertical { width: 12px; }
QToolTip { background: #293b49; color: #fff; border: 1px solid #567; }
'''


def main():
    app = QApplication(sys.argv)
    app.setApplicationName('Quadband Card Studio')
    app.setOrganizationName('Midwest Gadgets')
    app.setStyle('Fusion')
    window = MainWindow()
    window.show()
    if '--smoke-test' in sys.argv:
        # Construct all workspaces and shut down cleanly without discovering readers.
        require(window.stack.count() == 6, 'Workspace construction failed.')
        QTimer.singleShot(50, window.begin_close)
    return app.exec()


if __name__ == '__main__':
    sys.exit(main())
