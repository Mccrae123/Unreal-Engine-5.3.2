# Copyright Epic Games, Inc. All Rights Reserved.
from . import message_protocol
from .switchboard_logging import LOGGER

import datetime, select, socket, uuid, traceback

from collections import deque
from threading import Thread


class ListenerClient(object):
    def __init__(self, ip_address, port, buffer_size=1024):
        self.ip_address = ip_address
        self.port = port
        self.buffer_size = buffer_size

        self.message_queue = deque()
        self.close_socket = False

        self.socket = None
        self.handle_connection_thread = None

        #TODO: Consider converting these delegates to Signals and sending dict.

        self.disconnect_delegate = None

        self.command_accepted_delegate = None
        self.command_declined_delegate = None

        self.program_started_delegate = None
        self.program_start_failed_delegate = None
        self.program_ended_delegate = None
        self.program_killed_delegate = None
        self.program_kill_failed_delegate = None

        self.vcs_init_completed_delegate = None
        self.vcs_init_failed_delegate = None
        self.vcs_report_revision_completed_delegate = None
        self.vcs_report_revision_failed_delegate = None
        self.vcs_sync_completed_delegate = None
        self.vcs_sync_failed_delegate = None

        self.send_file_completed_delegate = None
        self.send_file_failed_delegate = None
        self.receive_file_completed_delegate = None
        self.receive_file_failed_delegate = None
        self.get_sync_status_delegate = None

        self.delegates = {
            "state" : None,
            "get sync status" : None,
        }

        self.last_activity = datetime.datetime.now()

    @property
    def server_address(self):
        if self.ip_address:
            return (self.ip_address, self.port)
        return None

    @property
    def is_connected(self):
        # I ran into an issue where running disconnect in a thread was causing the socket maintain it's reference
        # But self.socket.getpeername() fails because socket is sent to none. I am assuming that is due
        # it python's threading. Adding a try except to handle this
        try:
            if self.socket and self.socket.getpeername():
                return True
        except:
            return False
        return False

    @is_connected.setter
    def is_connected(self, value):
        if value == self.is_connected:
            return

        if value:
            self.connect()
        else:
            self.disconnect()

    def connect(self, ip_address=None):
        if ip_address:
            self.ip_address = ip_address
        elif not self.ip_address:
            LOGGER.debug('No ip_address was given. Cannot connect')
            return False

        self.close_socket = False
        self.last_activity = datetime.datetime.now()

        try:
            LOGGER.info(f"Connecting to {self.ip_address}:{self.port}")
            self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.socket.connect(self.server_address)

            # Create a thread that waits for messages from the server
            self.handle_connection_thread = Thread(target=self.handle_connection)
            self.handle_connection_thread.start()
        except OSError:
            LOGGER.error(f"Socket Error: {self.ip_address}:{self.port}")
            self.socket = None
            return False

        return True

    def disconnect(self, unexpected=False, exception=None):
        if self.is_connected:
            _, msg = message_protocol.create_disconnect_message()
            self.send_message(msg)

            self.close_socket = True
            self.handle_connection_thread.join()

    def handle_connection(self):
        buffer = []
        while self.is_connected:
            try:
                read_sockets, write_sockets, _ = select.select([self.socket], [self.socket], [], 0)

                if len(self.message_queue) > 0:
                    message_bytes = self.message_queue.pop()
                    for ws in write_sockets:
                        ws.send(message_bytes)
                        self.last_activity = datetime.datetime.now()

                received_data = []
                for rs in read_sockets:
                    received_data = rs.recv(self.buffer_size).decode()
                    self.process_received_data(buffer, received_data)
                    self.last_activity = datetime.datetime.now()

                delta = datetime.datetime.now() - self.last_activity
                if delta.total_seconds() > 2:
                    _, msg = message_protocol.create_keep_alive_message()
                    self.send_message(msg)

                if self.close_socket:
                    self.socket.shutdown(socket.SHUT_RDWR)
                    self.socket.close()
                    if self.disconnect_delegate:
                        self.disconnect_delegate(unexpected=False, exception=None)
                    self.socket = None
                    break

            except ConnectionResetError as e:
                self.socket.shutdown(socket.SHUT_RDWR)
                self.socket.close()
                self.socket = None
                if self.disconnect_delegate:
                    self.disconnect_delegate(unexpected=True, exception=e)
                return # todo: this needs to send a signal back to the main thread so the thread can be joined
            except OSError as e: # likely a socket error, so self.socket is not usuable any longer
                self.socket = None
                if self.disconnect_delegate:
                    self.disconnect_delegate(unexpected=True, exception=e)
                return

    def route_message(self, message):
        ''' Routes the received message to its delegate
        '''

        delegate = self.delegates.get(message['command'], None)

        if delegate:
            delegate(message)
            return

        if "command accepted" in message:
            message_id = uuid.UUID(message['id'])
            if message['command accepted'] == True:
                if self.command_accepted_delegate:
                    self.command_accepted_delegate(message_id)
            else:
                if self.command_declined_delegate:
                    self.command_declined_delegate(message_id, message["error"])

        elif "program started" in message:
            message_id = uuid.UUID(message['message id'])
            if message['program started'] == True:
                program_id = uuid.UUID(message['program id'])
                if self.program_started_delegate:
                    self.program_started_delegate(program_id, message_id)
            else:
                if self.program_start_failed_delegate:
                    self.program_start_failed_delegate(message['error'], message_id)

        elif "program ended" in message:
            program_id = uuid.UUID(message['program id'])
            if self.program_ended_delegate:
                self.program_ended_delegate(program_id, message['returncode'], message['output'])

        elif "program killed" in message:
            program_id = uuid.UUID(message['program id'])
            if message['program killed'] == True:
                if self.program_killed_delegate:
                    self.program_killed_delegate(program_id)
            else:
                if self.program_kill_failed_delegate:
                    self.program_kill_failed_delegate(program_id, message['error'])

        elif "vcs init complete" in message:
            if message['vcs init complete'] == True:
                if self.vcs_init_completed_delegate:
                    self.vcs_init_completed_delegate()
            else:
                if self.vcs_init_failed_delegate:
                    self.vcs_init_failed_delegate(message['error'])

        elif "vcs report revision complete" in message:
            if message['vcs report revision complete'] == True:
                if self.vcs_report_revision_completed_delegate:
                    self.vcs_report_revision_completed_delegate(message['revision'])
            else:
                if self.vcs_report_revision_failed_delegate:
                    self.vcs_report_revision_failed_delegate(message['error'])

        elif "vcs sync complete" in message:
            if message['vcs sync complete'] == True:
                if self.vcs_sync_completed_delegate:
                    self.vcs_sync_completed_delegate(message['revision'])
            else:
                if self.vcs_sync_failed_delegate:
                    self.vcs_sync_failed_delegate(message['error'])

        elif "send file complete" in message:
            if message['send file complete'] == True:
                if self.send_file_completed_delegate:
                    self.send_file_completed_delegate(message['destination'])
            else:
                if self.send_file_failed_delegate:
                    self.send_file_failed_delegate(message['destination'], message['error'])
                    
        elif "receive file complete" in message:
            if message['receive file complete'] == True:
                if self.receive_file_completed_delegate:
                    self.receive_file_completed_delegate(message['source'], message['content'])
            else:
                if self.receive_file_failed_delegate:
                    self.receive_file_failed_delegate(message['source'], message['error'])
        else:
            raise ValueError

    def process_received_data(self, buffer, received_data):

        for symbol in received_data:

            buffer.append(symbol)

            if symbol == '\x00': # found message end

                buffer.pop() # remove terminator
                message = message_protocol.decode_message(buffer)
                buffer.clear()

                # route message to its assigned delegate
                try:
                    self.route_message(message)
                except:
                    LOGGER.error(f"Error while parsing message: \n\n=== Traceback BEGIN ===\n{traceback.format_exc()}=== Traceback END ===\n")


    def send_message(self, message_bytes):
        if self.is_connected:
            LOGGER.message(f'Message: Sending ({self.ip_address}): {message_bytes}')
            self.message_queue.appendleft(message_bytes)
        else:
            LOGGER.error(f'Message: Failed to send ({self.ip_address}): {message_bytes}. No socket connected')
            if self.disconnect_delegate:
                self.disconnect_delegate(unexpected=True, exception=None)
