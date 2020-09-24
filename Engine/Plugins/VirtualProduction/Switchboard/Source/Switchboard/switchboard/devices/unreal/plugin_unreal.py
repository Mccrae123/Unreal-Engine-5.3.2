# Copyright Epic Games, Inc. All Rights Reserved.
from switchboard import config_osc as osc
from switchboard import message_protocol
from switchboard.config import CONFIG, Setting, SETTINGS
from switchboard.devices.device_base import Device, DeviceStatus
from switchboard.devices.device_widget_base import DeviceWidget
from switchboard.listener_client import ListenerClient
from switchboard.switchboard_logging import LOGGER

from PySide2 import QtWidgets, QtGui, QtCore

import base64
import os
from pathlib import Path


class DeviceUnreal(Device):
    setting_buffer_size = Setting("buffer_size", "Buffer Size", 1024, tool_tip="Buffer size used for communication with SwitchboardListener")
    setting_command_line_arguments = Setting("command_line_arguments", 'Command Line Arguments', "", tool_tip=f'Additional command line arguments for the engine')
    setting_is_recording_device = Setting("is_recording_device", 'Is Recording Device', True, tool_tip=f'Is this device used to record')
    setting_port = Setting("port", "Listener Port", 2980, tool_tip="Port of SwitchboardListener")
    setting_roles_filename = Setting("roles_filename", "Roles Filename", "VPRoles.ini", tool_tip="File that stores VirtualProduction roles. Default: Config/Tags/VPRoles.ini")
    setting_stage_session_id = Setting("stage_session_id", "Stage Session ID", 0)
    setting_ue4_exe = Setting("editor_exe", "UE4 Editor filename", "UE4Editor.exe")

    def __init__(self, name, ip_address, **kwargs):
        super().__init__(name, ip_address, **kwargs)

        self.unreal_client = ListenerClient(self.ip_address, port=self.setting_port.get_value(self.name), buffer_size=self.setting_buffer_size.get_value(self.name))

        roles = kwargs["roles"] if "roles" in kwargs.keys() else []
        self.setting_roles = Setting("roles", "Roles", roles, possible_values=[], tool_tip="List of roles for this device")
        self.setting_ip_address.signal_setting_changed.connect(self.on_setting_ip_address_changed)
        self.setting_port.signal_setting_changed.connect(self.on_setting_port_changed)

        self.auto_connect = False
        self.start_build_after_sync = False

        self.inflight_changelist = 0

        # Set a delegate method if the device gets a disconnect signal
        self.unreal_client.disconnect_delegate = self.on_listener_disconnect
        self.unreal_client.program_started_delegate = self.on_program_started
        self.unreal_client.program_start_failed_delegate = self.on_program_start_failed
        self.unreal_client.program_ended_delegate = self.on_program_ended
        self.unreal_client.program_kill_failed_delegate = self.on_program_kill_failed
        self.unreal_client.receive_file_completed_delegate = self.on_file_received
        self.unreal_client.receive_file_failed_delegate = self.on_file_receive_failed

        self._remote_programs_start_queue = {} # key: message_id, name
        self._running_remote_program_names = {} # key: program id, value: program name
        self._running_remote_program_ids = {} # key: program name, value: program id

    @staticmethod
    def plugin_settings():
        settings = Device.plugin_settings()
        settings.append(DeviceUnreal.setting_buffer_size)
        settings.append(DeviceUnreal.setting_command_line_arguments)
        settings.append(DeviceUnreal.setting_port)
        settings.append(DeviceUnreal.setting_roles_filename)
        settings.append(DeviceUnreal.setting_stage_session_id)
        settings.append(DeviceUnreal.setting_ue4_exe)
        return settings

    def setting_overrides(self):
        overrides = super().setting_overrides()
        overrides.append(Device.setting_is_recording_device)
        overrides.append(DeviceUnreal.setting_command_line_arguments)
        overrides.append(CONFIG.ENGINE_DIR)
        overrides.append(CONFIG.BUILD_ENGINE)
        overrides.append(CONFIG.SOURCE_CONTROL_WORKSPACE)
        overrides.append(CONFIG.UPROJECT_PATH)
        return overrides

    def device_settings(self):
        return super().device_settings() + [self.setting_roles]

    @property
    def category_name(self):
        if self.is_recording_device and self.status >= DeviceStatus.READY:
            return "Recording"
        return "Multiuser"

    def on_setting_ip_address_changed(self, _, new_address):
        LOGGER.info(f"Updating IP address for ListenerClient to {new_address}")
        self.unreal_client.ip_address = new_address

    def on_setting_port_changed(self, _, new_port):
        if not self.setting_port.is_overriden(self.name):
            LOGGER.info(f"Updating Port for ListenerClient to {new_port}")
            self.unreal_client.port = new_port

    def set_slate(self, value):
        if not self.is_recording_device:
            return

        self.send_osc_message(osc.SLATE, value)

    def set_take(self, value):
        if not self.is_recording_device:
            return

        self.send_osc_message(osc.TAKE, value)

    def record_stop(self):
        if not self.is_recording_device:
            return

        self.send_osc_message(osc.RECORD_STOP, 1)

    def connect_listener(self):
        is_connected = self.unreal_client.connect()

        if not is_connected:
            self.device_qt_handler.signal_device_connect_failed.emit(self)
            return

        super().connect_listener()

        self._request_roles_file()
        self._request_current_changelist_number()

    def _request_roles_file(self):
        uproject_path = CONFIG.UPROJECT_PATH.get_value(self.name)
        roles_filename = self.setting_roles_filename.get_value(self.name)
        roles_file_path = os.path.join(os.path.dirname(uproject_path), "Config", "Tags", roles_filename)
        _, msg = message_protocol.create_copy_file_from_listener_message(roles_file_path)
        self.unreal_client.send_message(msg)

    def _request_current_changelist_number(self):
        client_name = CONFIG.SOURCE_CONTROL_WORKSPACE.get_value(self.name)
        p4_path = CONFIG.P4_PATH.get_value()
        args = f'-F "%change%" -c {client_name} cstat {p4_path}/...#have'

        mid, msg = message_protocol.create_start_process_message("p4", args)
        self._remote_programs_start_queue[mid] = "cstat"

        self.unreal_client.send_message(msg)

    def disconnect_listener(self):
        super().disconnect_listener()
        self.unreal_client.disconnect()

    def sync(self, changelist):
        project_name = os.path.basename(os.path.dirname(CONFIG.UPROJECT_PATH.get_value(self.name)))
        LOGGER.info(f"{self.name}: Syncing project {project_name} to revision {changelist}")
        self.inflight_changelist = changelist

        sync_tool = ""
        sync_args = ""
        # for installed/vanilla engine we directly call p4 to sync the project itself. RunUAT only works when the engine itself is in p4.
        engine_needs_building = CONFIG.BUILD_ENGINE.get_value(self.name)
        if engine_needs_building:
            sync_tool = f'{os.path.normpath(os.path.join(CONFIG.ENGINE_DIR.get_value(self.name), "Build", "BatchFiles", "RunUAT.bat"))}'
            sync_args = f'-P4 SyncProject -project="{CONFIG.UPROJECT_PATH.get_value(self.name)}" -cl={changelist} -threads=8 -generate'
        else:
            p4_path = CONFIG.P4_PATH.get_value()
            workspace = CONFIG.SOURCE_CONTROL_WORKSPACE.get_value(self.name)
            sync_tool = "p4"
            sync_args = f"-c{workspace} sync {p4_path}/...@{changelist}"

        LOGGER.info(f"{self.name}: Sending sync command: {sync_tool} {sync_args}")
        mid, msg = message_protocol.create_start_process_message(sync_tool, sync_args)
        self._remote_programs_start_queue[mid] = "sync"
        self.unreal_client.send_message(msg)
        self.status = DeviceStatus.SYNCING

    def build(self):
        if self.status == DeviceStatus.SYNCING:
            self.start_build_after_sync = True
            LOGGER.info(f"{self.name}: Build scheduled to start after successful sync operation")
            return
        engine_path = CONFIG.ENGINE_DIR.get_value(self.name)
        build_tool = os.path.join(engine_path, "Binaries", "DotNET", "UnrealBuildTool")
        build_args = f"UE4Editor Win64 Development {CONFIG.UPROJECT_PATH.get_value(self.name)} -progress"
        mid, msg = message_protocol.create_start_process_message(build_tool, build_args)
        self._remote_programs_start_queue[mid] = "build"
        LOGGER.info(f"{self.name}: Sending build command: {build_tool} {build_args}")
        self.unreal_client.send_message(msg)
        self.status = DeviceStatus.BUILDING

    def close(self, force=False):
        try:
            program_id = self._running_remote_program_ids["unreal"]
            _, msg = message_protocol.create_kill_process_message(program_id)
            self.unreal_client.send_message(msg)
        except KeyError:
            self.status = DeviceStatus.CLOSED

    def generate_unreal_exe_path(self):
        return CONFIG.engine_path(CONFIG.ENGINE_DIR.get_value(self.name), self.setting_ue4_exe.get_value())

    def generate_unreal_command_line_args(self, map_name):
        command_line_args = f'{self.setting_command_line_arguments.get_value(self.name)}'
        if CONFIG.MUSERVER_AUTO_JOIN:
            command_line_args += f' -CONCERTAUTOCONNECT -CONCERTSERVER={CONFIG.MUSERVER_SERVER_NAME} -CONCERTSESSION={SETTINGS.MUSERVER_SESSION_NAME} -CONCERTDISPLAYNAME={self.name}'
        
        selected_roles = self.setting_roles.get_value()
        unsupported_roles = [role for role in selected_roles if role not in self.setting_roles.possible_values]
        supported_roles = [role for role in selected_roles if role not in unsupported_roles]

        if supported_roles:
            command_line_args += ' -VPRole=' + '|'.join(supported_roles)
        if unsupported_roles:
            LOGGER.error(f"{self.name}: Omitted unsupported roles: {'|'.join(unsupported_roles)}")

        session_id = self.setting_stage_session_id.get_value()
        if session_id > 0:
            command_line_args += f" -StageSessionId={session_id}"
        command_line_args += f" -StageFriendlyName={self.name}"

        args = f'"{CONFIG.UPROJECT_PATH.get_value(self.name)}" {map_name} {command_line_args}'
        return args

    def generate_unreal_command_line(self, map_name):
        return self.generate_unreal_exe_path(), self.generate_unreal_command_line_args(map_name)

    def launch(self, map_name):
        engine_path, args = self.generate_unreal_command_line(map_name)
        LOGGER.info(f"Launching UE4: {engine_path} {args}")

        mid, msg = message_protocol.create_start_process_message(engine_path, args)
        self._remote_programs_start_queue[mid] = "unreal"

        self.unreal_client.send_message(msg)

    def on_listener_disconnect(self, unexpected=False, exception=None):
        if unexpected:
            self.device_qt_handler.signal_device_client_disconnected.emit(self)

    def on_program_started(self, program_id, message_id):
        program_name = self._remote_programs_start_queue.pop(message_id)
        LOGGER.info(f"{self.name}: {program_name} with id {program_id} was successfully started")
        if program_name == "unreal":
            self.status = DeviceStatus.OPEN
            self.send_osc_message(osc.OSC_ADD_SEND_TARGET, [SETTINGS.IP_ADDRESS, CONFIG.OSC_SERVER_PORT])

        self._running_remote_program_names[program_id] = program_name
        self._running_remote_program_ids[program_name] = program_id

    def on_program_start_failed(self, error, message_id):
        program_name = self._remote_programs_start_queue.pop(message_id)
        LOGGER.error(f"Could not start {program_name}: {error}")
        if program_name in ["sync", "build"]:
            self.status = DeviceStatus.CLOSED
            self.changelist = self.changelist # force to show existing changelist to hide building/syncing

    def on_program_ended(self, program_id, returncode, output):
        LOGGER.info(f"{self.name}: Program with id {program_id} exited with returncode {returncode}")
        program_name = self._running_remote_program_names.pop(program_id)
        self._running_remote_program_ids.pop(program_name)
        if program_name == "unreal":
            self.status = DeviceStatus.CLOSED
        elif program_name == "sync":
            self.status = DeviceStatus.CLOSED
            if returncode != 0:
                LOGGER.error(f"{self.name}: Project was not synced successfully!")
                for line in output.splitlines():
                    LOGGER.error(f"{self.name}: {line}")
                self.device_qt_handler.signal_device_sync_failed.emit(self)
            else:
                LOGGER.info(f"{self.name}: Project was synced successfully")
                self.changelist = self.inflight_changelist
                if self.start_build_after_sync:
                    self.start_build_after_sync = False
                    self.build()
            self.inflight_changelist = 0
        elif program_name == "build":
            if returncode == 0:
                LOGGER.info(f"{self.name}: Project was built successfully!")
            else:
                LOGGER.error(f"{self.name}: Project was not built successfully!")
                for line in output.splitlines():
                    LOGGER.error(f"{self.name}: {line}")
            self.status = DeviceStatus.CLOSED
            self.changelist = self.changelist # forces an update to the changelist field (to hide the Building state)
        elif program_name == "cstat":
            changelists = [line.strip() for line in output.split()]
            if changelists:
                current_changelist = changelists[-1]
                project_name = os.path.basename(os.path.dirname(CONFIG.UPROJECT_PATH.get_value(self.name)))
                LOGGER.info(f"{self.name}: Project {project_name} is on revision {current_changelist}")
                self.changelist = current_changelist
            else:
                LOGGER.error(f"{self.name}: Could not retrieve changelists for project. Are the Source Control Settings correctly configured?")

    def on_program_kill_failed(self, program_id, error):
        self._running_remote_program_ids["unreal"].pop(program_id)
        LOGGER.error(f"Unable to close Unreal with id {str(program_id)}")
        LOGGER.error(f"Error: {error}")
        self.status = DeviceStatus.CLOSED

    def on_file_received(self, source_path, content):
        if source_path.endswith(self.setting_roles_filename.get_value(self.name)):
            decoded_content = base64.b64decode(content).decode()
            tags = parse_unreal_tag_file(decoded_content.splitlines())
            self.setting_roles.possible_values = tags
            LOGGER.info(f"{self.name}: All possible roles: {tags}")
            unsupported_roles = [role for role in self.setting_roles.get_value() if role not in tags]
            if len(unsupported_roles) > 0:
                LOGGER.error(f"{self.name}: Found unsupported roles: {unsupported_roles}")
                LOGGER.error(f"{self.name}: Please change the roles for this device in the settings or in the unreal project settings!")

    def on_file_receive_failed(self, source_path, error):
        roles = self.setting_roles.get_value()
        if len(roles) > 0:
            LOGGER.error(f"{self.name}: Error receiving role file from listener and device claims to have these roles: {' | '.join(roles)}")
            LOGGER.error(f"Error: {error}")

    def transport_paths(self, device_recording):
        """
        Do not transport UE4 paths as they will be checked into source control
        """
        return []


def parse_unreal_tag_file(file_content):
    tags = []
    for line in file_content:
        LOGGER.info(line)
        if line.startswith("GameplayTagList"):
            tag = line.split("Tag=")[1]
            tag = tag.split(',', 1)[0]
            tag = tag.strip('"')
            tags.append(tag)
    return tags


class DeviceWidgetUnreal(DeviceWidget):
    def __init__(self, name, device_hash, ip_address, icons, parent=None):
        super().__init__(name, device_hash, ip_address, icons, parent=parent)

    def _add_control_buttons(self):
        super()._add_control_buttons()

        self.changelist_label = QtWidgets.QLabel()
        self.changelist_label.setFont(QtGui.QFont("Roboto", 10))
        spacer = QtWidgets.QSpacerItem(0, 20, QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Minimum)

        self.layout.addWidget(self.changelist_label)

        self.sync_button = self.add_control_button(':/icons/images/icon_sync.png',
                                                    icon_disabled=':/icons/images/icon_sync_disabled.png',
                                                    icon_hover=':/icons/images/icon_sync_hover.png',
                                                    checkable=False, tool_tip='Sync Changelist')
        self.build_button = self.add_control_button(':/icons/images/icon_build.png',
                                                    icon_disabled=':/icons/images/icon_build_disabled.png',
                                                    icon_hover=':/icons/images/icon_build_hover.png',
                                                    icon_size=QtCore.QSize(21, 21),
                                                    checkable=False, tool_tip='Build Changelist')
        self.layout.addItem(spacer)

        self.open_button = self.add_control_button(':/icons/images/icon_open.png',
                                                    icon_hover=':/icons/images/icon_open_hover.png',
                                                    icon_disabled=':/icons/images/icon_open_disabled.png',
                                                    icon_on=':/icons/images/icon_close.png',
                                                    icon_hover_on=':/icons/images/icon_close_hover.png',
                                                    icon_disabled_on=':/icons/images/icon_close_disabled.png',
                                                    tool_tip='Start Unreal')

        self.connect_button = self.add_control_button(':/icons/images/icon_connect.png',
                                                        icon_hover=':/icons/images/icon_connect_hover.png',
                                                        icon_disabled=':/icons/images/icon_connect_disabled.png',
                                                        icon_on=':/icons/images/icon_connected.png',
                                                        icon_hover_on=':/icons/images/icon_connected_hover.png',
                                                        icon_disabled_on=':/icons/images/icon_connected_disabled.png',
                                                        tool_tip='Connect to listener')

        self.sync_button.clicked.connect(self.sync_button_clicked)
        self.build_button.clicked.connect(self.build_button_clicked)
        self.connect_button.clicked.connect(self.connect_button_clicked)
        self.open_button.clicked.connect(self.open_button_clicked)

        # Disable UI when not connected
        self.open_button.setDisabled(True)
        self.sync_button.setDisabled(True)
        self.build_button.setDisabled(True)

        self.changelist_label.hide()
        self.sync_button.hide()
        self.build_button.hide()

    def can_sync(self):
        return self.sync_button.isEnabled()

    def can_build(self):
        return self.build_button.isEnabled()

    def _open(self):
        # Make sure the button is in the correct state
        self.open_button.setChecked(True)
        # Emit Signal to Switchboard
        self.signal_device_widget_open.emit(self)

    def _close(self):
        # Make sure the button is in the correct state
        self.open_button.setChecked(False)
        # Emit Signal to Switchboard
        self.signal_device_widget_close.emit(self)

    def _connect(self):
        # Make sure the button is in the correct state
        self.connect_button.setChecked(True)
        self.connect_button.setToolTip("Disconnect from listener")

        self.open_button.setDisabled(False)
        self.sync_button.setDisabled(False)
        self.build_button.setDisabled(False)

        # Emit Signal to Switchboard
        self.signal_device_widget_connect.emit(self)

    def _disconnect(self):
        # Make sure the button is in the correct state
        self.connect_button.setChecked(False)
        self.connect_button.setToolTip("Connect to listener")

        # Don't show the changelist
        self.changelist_label.hide()
        self.sync_button.hide()
        self.build_button.hide()

        # Disable the buttons
        self.open_button.setDisabled(True)
        self.sync_button.setDisabled(True)
        self.build_button.setDisabled(True)

        # Emit Signal to Switchboard
        self.signal_device_widget_disconnect.emit(self)

    def update_status(self, status, previous_status):
        super().update_status(status, previous_status)

        #self.changelist_label.setText('')

        if status == DeviceStatus.CLOSED:
            self.open_button.setDisabled(False)
            self.open_button.setChecked(False)
            self.sync_button.setDisabled(False)
            self.build_button.setDisabled(False)
        elif status == DeviceStatus.SYNCING:
            self.open_button.setDisabled(True)
            self.sync_button.setDisabled(True)
            self.build_button.setDisabled(True)
            self.changelist_label.setText('Syncing')
        elif status == DeviceStatus.BUILDING:
            self.open_button.setDisabled(True)
            self.sync_button.setDisabled(True)
            self.build_button.setDisabled(True)
            self.changelist_label.setText('Building')
        elif status == DeviceStatus.OPEN:
            self.open_button.setDisabled(False)
            self.open_button.setChecked(True)
            self.sync_button.setDisabled(True)
            self.build_button.setDisabled(True)
        elif status == DeviceStatus.READY:
            self.open_button.setDisabled(False)
            self.open_button.setChecked(True)
            self.sync_button.setDisabled(True)
            self.build_button.setDisabled(True)

        if self.open_button.isChecked():
            self.open_button.setToolTip("Stop Unreal")
        else:
            self.open_button.setToolTip("Start Unreal")

    def update_changelist(self, value):
        self.changelist_label.setText(value)

        self.changelist_label.show()
        self.sync_button.show()
        self.build_button.show()

    def changelist_display_warning(self, b):
        if b:
            self.changelist_label.setProperty("error", True)
        else:
            self.changelist_label.setProperty("error", False)
        self.changelist_label.setStyle(self.changelist_label.style())

    def sync_button_clicked(self):
        self.signal_device_widget_sync.emit(self)

    def build_button_clicked(self):
        self.signal_device_widget_build.emit(self)

    def open_button_clicked(self):
        if self.open_button.isChecked():
            self._open()
        else:
            self._close()

    def connect_button_clicked(self):
        if self.connect_button.isChecked():
            self._connect()
        else:
            self._disconnect()

