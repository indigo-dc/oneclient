

import random

__author__ = "Konrad Zemek"
__copyright__ = """(C) 2015 ACK CYFRONET AGH,
This software is released under the MIT license cited in 'LICENSE.txt'."""

import os
import sys
from threading import Thread
from multiprocessing import Pool
import time
import math
import json
import pytest
from stat import *
import xml.etree.ElementTree as ET

script_dir = os.path.dirname(os.path.realpath(__file__))
sys.path.insert(0, os.path.dirname(script_dir))
from test_common import *
# noinspection PyUnresolvedReferences
from environment import appmock, common, docker
# noinspection PyUnresolvedReferences
import fslogic
# noinspection PyUnresolvedReferences
from proto import messages_pb2, fuse_messages_pb2, event_messages_pb2, \
    common_messages_pb2, stream_messages_pb2

SYNCHRONIZE_BLOCK_PRIORITY_IMMEDIATE = 32

def prepare_status_response():
    server_response = messages_pb2.ServerMessage()
    server_response.fuse_response.status.code = common_messages_pb2.Status.ok

    return server_response


@pytest.fixture
def endpoint(appmock_client):
    app = appmock_client.tcp_endpoint(443)
    yield app
    appmock_client.reset_tcp_history()


@pytest.fixture(scope="function")
def fl(endpoint):
    fsl = fslogic.FsLogicProxy(endpoint.ip, endpoint.port, 10000, 5*60, "")

    with reply(endpoint, [prepare_status_response()]) as queue:
        fsl.start()
        queue.get()

    yield fsl
    fsl.stop()


@pytest.fixture
def fl_dircache(endpoint):
    fsl = fslogic.FsLogicProxy(endpoint.ip, endpoint.port,
            25, # Max metadata cache size
            3,   # Directory cache expires after 3 seconds
            "")

    with reply(endpoint, [prepare_status_response()]) as queue:
        fsl.start()
        queue.get()

    yield fsl
    fsl.stop()


@pytest.fixture
def fl_archivematica(endpoint):
    fsl = fslogic.FsLogicProxy(endpoint.ip, endpoint.port,
            10000, 5*60, "--enable-archivematica")
    yield fsl
    fsl.stop()


@pytest.fixture
def fl_onlyfullreplicas(endpoint):
    fsl = fslogic.FsLogicProxy(endpoint.ip, endpoint.port,
            10000, 5*60, "--only-full-replicas")
    yield fsl
    fsl.stop()

@pytest.fixture
def uuid():
    return random_str()


@pytest.fixture
def parentUuid():
    return random_str()


@pytest.fixture
def stat(endpoint, fl, uuid):
    response = prepare_attr_response(uuid, fuse_messages_pb2.REG)
    with reply(endpoint, response):
        return fl.getattr(uuid)


@pytest.fixture
def parentStat(endpoint, fl, parentUuid):
    response = prepare_attr_response(parentUuid, fuse_messages_pb2.DIR)
    with reply(endpoint, response):
        return fl.getattr(parentUuid)


def prepare_file_blocks(blocks=[]):
    file_blocks = []
    for file_block in blocks:
        block = common_messages_pb2.FileBlock()
        if len(file_block) == 2:
            offset, block_size = file_block
        else:
            offset, block_size, storage_id, file_id = file_block
            block.storage_id = storage_id.encode('utf-8')
            block.file_id = file_id.encode('utf-8')
        block.offset = offset
        block.size = block_size
        file_blocks.append(block)
    return file_blocks


def prepare_sync_response(uuid, data, blocks):
    location = prepare_location(uuid, blocks)

    server_response = messages_pb2.ServerMessage()
    server_response.fuse_response.file_location_changed.file_location.CopyFrom(location)
    server_response.fuse_response.status.code = common_messages_pb2.Status.ok

    return server_response


def prepare_sync_and_checksum_response(uuid, data, blocks, checksum):
    location = prepare_location(uuid, blocks)

    server_response = messages_pb2.ServerMessage()
    server_response.fuse_response.sync_response.checksum = checksum.encode('utf-8')
    server_response.fuse_response.sync_response.file_location_changed.file_location.CopyFrom(location)
    server_response.fuse_response.status.code = common_messages_pb2.Status.ok

    return server_response


def prepare_partial_sync_response(uuid, data, blocks, start, end):
    location = prepare_location(uuid, blocks)

    server_response = messages_pb2.ServerMessage()
    server_response.fuse_response.file_location_changed.file_location.CopyFrom(location)
    server_response.fuse_response.file_location_changed.change_beg_offset = start
    server_response.fuse_response.file_location_changed.change_end_offset = end
    server_response.fuse_response.status.code = common_messages_pb2.Status.ok

    return server_response


def prepare_sync_eagain_response(uuid, data, blocks):
    location = prepare_location(uuid, blocks)

    server_response = messages_pb2.ServerMessage()
    server_response.fuse_response.file_location.CopyFrom(location)
    server_response.fuse_response.status.code = common_messages_pb2.Status.eagain

    return server_response


def prepare_sync_ecanceled_response(uuid, data, blocks):
    location = prepare_location(uuid, blocks)

    server_response = messages_pb2.ServerMessage()
    server_response.fuse_response.file_location.CopyFrom(location)
    server_response.fuse_response.status.code = common_messages_pb2.Status.ecanceled

    return server_response


def prepare_sync_request(uuid, offset, size):
    block = common_messages_pb2.FileBlock()
    block.offset = offset
    block.size = size

    req = fuse_messages_pb2.SynchronizeBlock()
    req.uuid = uuid.encode('utf-8')
    req.block.CopyFrom(block)

    client_request = messages_pb2.ClientMessage()
    client_request.fuse_request.synchronize_block.CopyFrom(req)

    return client_request


def prepare_sync_and_fetch_checksum_request(uuid, offset, size):
    block = common_messages_pb2.FileBlock()
    block.offset = offset
    block.size = size

    req = fuse_messages_pb2.SynchronizeBlockAndComputeChecksum()
    req.uuid = uuid.encode('utf-8')
    req.block.CopyFrom(block)

    client_request = messages_pb2.ClientMessage()
    client_request.fuse_request.synchronize_block_and_compute_checksum.CopyFrom(req)

    return client_request


def prepare_attr_response(uuid, filetype, size=None, parent_uuid=None, name='filename'):
    repl = fuse_messages_pb2.FileAttr()
    repl.uuid = uuid.encode('utf-8')
    if parent_uuid:
        repl.parent_uuid = parent_uuid if isinstance(parent_uuid, bytes) else parent_uuid.encode('utf-8')
    repl.name = name.encode('utf-8')
    repl.mode = random.randint(0, 1023)
    repl.uid = random.randint(0, 20000)
    repl.gid = random.randint(0, 20000)
    repl.mtime = int(time.time()) - random.randint(0, 1000000)
    repl.atime = int(time.time()) - random.randint(0, 1000000)
    repl.ctime = int(time.time()) - random.randint(0, 1000000)
    repl.type = filetype
    repl.size = size if size else random.randint(0, 1000000000)
    repl.owner_id = b''
    repl.provider_id = b''
    repl.index = b''

    server_response = messages_pb2.ServerMessage()
    server_response.fuse_response.file_attr.CopyFrom(repl)
    server_response.fuse_response.status.code = common_messages_pb2.Status.ok

    return server_response


def prepare_attr_response_mode(uuid, filetype, mode, parent_uuid=None):
    repl = fuse_messages_pb2.FileAttr()
    repl.uuid = uuid.encode('utf-8')
    if parent_uuid:
        repl.parent_uuid = parent_uuid.encode('utf-8')
    repl.name = 'filename'.encode('utf-8')
    repl.mode = mode
    repl.uid = random.randint(0, 20000)
    repl.gid = random.randint(0, 20000)
    repl.mtime = int(time.time()) - random.randint(0, 1000000)
    repl.atime = int(time.time()) - random.randint(0, 1000000)
    repl.ctime = int(time.time()) - random.randint(0, 1000000)
    repl.type = filetype
    repl.owner_id = b''
    repl.provider_id = b''
    repl.index = b''

    server_response = messages_pb2.ServerMessage()
    server_response.fuse_response.file_attr.CopyFrom(repl)
    server_response.fuse_response.status.code = common_messages_pb2.Status.ok

    return server_response


def prepare_readlink_response(uuid, link):
    repl = fuse_messages_pb2.Symlink()

    repl.link = link.encode('utf-8')

    server_response = messages_pb2.ServerMessage()
    server_response.fuse_response.symlink.CopyFrom(repl)
    server_response.fuse_response.status.code = common_messages_pb2.Status.ok

    return server_response


def prepare_fsstat_response(uuid, space_id, storage_count=1, size=1024*1024, occupied=1024):
    server_response = messages_pb2.ServerMessage()
    server_response.fuse_response.fs_stats.space_id = space_id.encode('utf-8')
    storages = []
    for i in range(0, storage_count):
        storage = fuse_messages_pb2.StorageStats()
        storage.storage_id = ("storage_"+str(i)).encode('utf-8')
        storage.size = size
        storage.occupied = occupied
        storages.append(storage)

    server_response.fuse_response.fs_stats.storage_stats.extend(storages)
    server_response.fuse_response.status.code = common_messages_pb2.Status.ok

    return server_response

def prepare_helper_response():
    repl = fuse_messages_pb2.HelperParams()
    repl.helper_name = b'null'

    server_response = messages_pb2.ServerMessage()
    server_response.fuse_response.helper_params.CopyFrom(repl)
    server_response.fuse_response.status.code = common_messages_pb2.Status.ok

    return server_response


def prepare_location(uuid, blocks=[]):
    file_blocks = prepare_file_blocks(blocks)

    repl = fuse_messages_pb2.FileLocation()
    repl.uuid = uuid.encode('utf-8')
    repl.space_id = b'space1'
    repl.storage_id = b'storage1'
    repl.file_id = b'file1'
    repl.provider_id = b'provider1'
    repl.blocks.extend(file_blocks)
    repl.version = 1

    return repl


def prepare_location_response(uuid, blocks=[]):
    location = prepare_location(uuid, blocks)

    server_response = messages_pb2.ServerMessage()
    server_response.fuse_response.file_location.CopyFrom(location)
    server_response.fuse_response.status.code = common_messages_pb2.Status.ok

    return server_response


def prepare_rename_response(new_uuid):
    repl = fuse_messages_pb2.FileRenamed()
    repl.new_uuid = new_uuid.encode('utf-8')

    server_response = messages_pb2.ServerMessage()
    server_response.fuse_response.file_renamed.CopyFrom(repl)
    server_response.fuse_response.status.code = common_messages_pb2.Status.ok

    return server_response


def prepare_processing_status_response(status):
    repl = messages_pb2.ProcessingStatus()
    repl.code = status

    server_response = messages_pb2.ServerMessage()
    server_response.processing_status.CopyFrom(repl)

    return server_response


def prepare_open_response(handle_id='handle_id'):
    repl = fuse_messages_pb2.FileOpened()
    repl.handle_id = handle_id.encode('utf-8')

    server_response = messages_pb2.ServerMessage()
    server_response.fuse_response.file_opened.CopyFrom(repl)
    server_response.fuse_response.status.code = common_messages_pb2.Status.ok

    return server_response


def prepare_file_children_attr_response(parent_uuid, prefix, count):
    child_attrs = []
    for i in range(count):
        f = prepare_attr_response(random_str(), fuse_messages_pb2.REG, 1, parent_uuid).\
                    fuse_response.file_attr
        f.name = (prefix+str(i)).encode('utf-8')
        child_attrs.append(f)

    response = fuse_messages_pb2.FileChildrenAttrs()
    response.child_attrs.extend(child_attrs)

    return response


def prepare_events(evt_list):
    evts = event_messages_pb2.Events()
    evts.events.extend(evt_list)

    msg = messages_pb2.ServerMessage()
    msg.events.CopyFrom(evts)

    return msg


def prepare_file_attr_changed_event(uuid, type, size, parent_uuid, mode=None):
    attr = fuse_messages_pb2.FileAttr()
    attr.uuid = uuid if isinstance(uuid, bytes) else uuid.encode('utf-8')
    attr.name = b'filename'
    attr.mode = mode if mode else random_int(upper_bound=0o777)
    attr.uid = random_int(upper_bound=20000)
    attr.gid = random_int(upper_bound=20000)
    attr.mtime = int(time.time()) - random_int(upper_bound=1000000)
    attr.atime = int(time.time()) - random_int(upper_bound=1000000)
    attr.ctime = int(time.time()) - random_int(upper_bound=1000000)
    attr.type = type
    if size:
        attr.size = size
    attr.owner_id = b''
    attr.provider_id = b''
    attr.parent_uuid = parent_uuid.encode('utf-8')
    attr.index = b''

    attr_evt = event_messages_pb2.FileAttrChangedEvent()
    attr_evt.file_attr.CopyFrom(attr)

    evt = event_messages_pb2.Event()
    evt.file_attr_changed.CopyFrom(attr_evt)

    return prepare_events([evt])


def prepare_file_renamed_event(uuid, new_uuid, new_name, new_parent_uuid):
    top_entry = common_messages_pb2.FileRenamedEntry()
    top_entry.old_uuid = uuid if isinstance(uuid, bytes) else uuid.encode('utf-8')
    top_entry.new_uuid = new_uuid.encode('utf-8')
    top_entry.new_name = new_name.encode('utf-8')
    top_entry.new_parent_uuid = new_parent_uuid.encode('utf-8')

    rename_evt = event_messages_pb2.FileRenamedEvent()
    rename_evt.top_entry.CopyFrom(top_entry)

    evt = event_messages_pb2.Event()
    evt.file_renamed.CopyFrom(rename_evt)

    return prepare_events([evt])


def do_open(endpoint, fl, uuid, size=None, blocks=[], handle_id='handle_id'):
    ok = prepare_status_response()
    attr_response = prepare_attr_response(uuid, fuse_messages_pb2.REG,
                                          size=size)
    location_response = prepare_location_response(uuid, blocks)
    open_response = prepare_open_response(handle_id)

    with reply(endpoint, [attr_response,
                          location_response,
                          ok, ok, ok, ok,
                          open_response]):
        handle = fl.open(uuid, 0)
        assert handle >= 0
        return handle


def do_open_cached(endpoint, fl, uuid, size=None, blocks=[], handle_id='handle_id'):
    ok = prepare_status_response()
    location_response = prepare_location_response(uuid, blocks)
    open_response = prepare_open_response(handle_id)

    with reply(endpoint, [location_response,
                          ok,
                          open_response]):
        handle = fl.open(uuid, 0)
        assert handle >= 0
        return handle


def do_release(endpoint, fl, uuid, fh):
    fsync_response = messages_pb2.ServerMessage()
    fsync_response.fuse_response.status.code = common_messages_pb2.Status.ok

    release_response = messages_pb2.ServerMessage()
    release_response.fuse_response.status.code = common_messages_pb2.Status.ok

    result = None
    with reply(endpoint, [fsync_response,
                          release_response]) as queue:
        fl.release(uuid, fh)
        result = queue
    return result


def get_stream_id_from_location_subscription(subscription_message_data):
    location_subsc = messages_pb2.ClientMessage()
    location_subsc.ParseFromString(subscription_message_data)
    return location_subsc.message_stream.stream_id


def test_statfs_should_get_storage_size(appmock_client, endpoint, fl, uuid):
    block_size = 4096

    ok = prepare_status_response()
    response = prepare_fsstat_response(uuid, "space_1", 1, 1000*block_size, 21*block_size)

    with reply(endpoint, [response]) as queue:
        statfs = fl.statfs(uuid)
        queue.get()

    assert statfs.bsize == block_size
    assert statfs.frsize == block_size
    assert statfs.blocks == 1000
    assert statfs.bavail == 1000-21


def test_statfs_should_report_empty_free_space_on_overoccupied_storage(appmock_client, endpoint, fl, uuid):
    block_size = 4096

    ok = prepare_status_response()
    response = prepare_fsstat_response(uuid, "space_1", 2, 10*block_size, 20*block_size)

    with reply(endpoint, [response]) as queue:
        statfs = fl.statfs(uuid)
        queue.get()

    assert statfs.bsize == block_size
    assert statfs.frsize == block_size
    assert statfs.blocks == 2*10
    assert statfs.bavail == 0


def test_getattrs_should_get_attrs(appmock_client, endpoint, fl, uuid, parentUuid):
    ok = prepare_status_response()
    response = prepare_attr_response(uuid, fuse_messages_pb2.REG, 1, parentUuid)
    parentParentUuid = random_str()
    parent_response = prepare_attr_response(parentUuid, fuse_messages_pb2.DIR, None, parentParentUuid)

    with reply(endpoint, [response,
                          parent_response]) as queue:
        stat = fl.getattr(uuid)
        client_message = queue.get()

    assert client_message.HasField('fuse_request')

    fuse_request = client_message.fuse_request
    assert fuse_request.file_request.HasField('get_file_attr')
    assert fuse_request.file_request.context_guid.decode('utf-8') == uuid

    repl = response.fuse_response.file_attr
    assert repl.uuid.decode('utf-8') == uuid
    assert stat.atime == repl.atime
    assert stat.mtime == repl.mtime
    assert stat.ctime == repl.ctime
    assert stat.gid == repl.gid
    assert stat.uid == repl.uid
    assert stat.mode == repl.mode | fslogic.regularMode()
    assert stat.size == repl.size


def test_getattrs_should_pass_errors(appmock_client, endpoint, fl, uuid):
    ok = prepare_status_response()
    response = messages_pb2.ServerMessage()
    response.fuse_response.status.code = common_messages_pb2.Status.enoent

    with pytest.raises(RuntimeError) as excinfo:
        with reply(endpoint, [response]):
            fl.getattr(uuid)

    assert 'No such file or directory' in str(excinfo.value)


def test_getattrs_should_cache_attrs(appmock_client, endpoint, fl, uuid, parentUuid):
    ok = prepare_status_response()
    attr_response = prepare_attr_response(uuid, fuse_messages_pb2.REG, 1, parentUuid)
    attr_parent_response = prepare_attr_response(parentUuid, fuse_messages_pb2.DIR)

    # FsLogic should first request the FileAttr for uuid, and then
    # call the FileAttr for it's parent since it isn't cached
    # After that it should create subscriptions on the parent for the
    # metadata changes in that directory
    with reply(endpoint, [attr_response, attr_parent_response]):
        stat = fl.getattr(uuid)

    assert fl.metadata_cache_contains(uuid)
    assert fl.metadata_cache_contains(parentUuid)

    # This should return the attr without any calls to Oneprovider
    new_stat = fl.getattr(uuid)

    assert stat == new_stat


def test_mkdir_should_mkdir(appmock_client, endpoint, fl):
    getattr_response = prepare_attr_response('parentUuid', fuse_messages_pb2.DIR)
    response = messages_pb2.ServerMessage()
    response.fuse_response.status.code = common_messages_pb2.Status.ok

    with reply(endpoint, [response, getattr_response]) as queue:
        fl.mkdir('parentUuid', 'name', 0o123)
        client_message = queue.get()

    assert client_message.HasField('fuse_request')
    assert client_message.fuse_request.HasField('file_request')

    file_request = client_message.fuse_request.file_request
    assert file_request.context_guid == b'parentUuid'

    assert file_request.HasField('create_dir')
    create_dir = file_request.create_dir
    assert create_dir.name == b'name'
    assert create_dir.mode == 0o123
    assert file_request.context_guid == \
           getattr_response.fuse_response.file_attr.uuid


def test_mkdir_should_recreate_dir(appmock_client, endpoint, fl):
    ok = prepare_status_response()
    getattr_response = prepare_attr_response('parentUuid', fuse_messages_pb2.DIR)

    def mkdir(getattr_response_param):

        with reply(endpoint, [ok,
                              getattr_response_param]) as queue:
            fl.mkdir('parentUuid', 'name', 0o123)
            client_message = queue.get()

        assert client_message.HasField('fuse_request')
        assert client_message.fuse_request.HasField('file_request')

        file_request = client_message.fuse_request.file_request
        assert file_request.context_guid == b'parentUuid'

        assert file_request.HasField('create_dir')
        create_dir = file_request.create_dir
        assert create_dir.name == b'name'
        assert create_dir.mode == 0o123
        assert file_request.context_guid == \
            getattr_response.fuse_response.file_attr.uuid

    mkdir(getattr_response)

    with reply(endpoint, [ok, ok, ok, getattr_response, ok]) as queue:
        fl.unlink('parentUuid', 'name')

    getattr_response2 = prepare_attr_response('parentUuid2', fuse_messages_pb2.DIR)
    mkdir(getattr_response2)


def test_mkdir_should_pass_mkdir_errors(appmock_client, endpoint, fl):
    getattr_response = prepare_attr_response('parentUuid', fuse_messages_pb2.DIR)
    response = messages_pb2.ServerMessage()
    response.fuse_response.status.code = common_messages_pb2.Status.eperm

    with pytest.raises(RuntimeError) as excinfo:
        with reply(endpoint, [getattr_response, response]):
            fl.mkdir('parentUuid', 'filename', 0o123)

    assert 'Operation not permitted' in str(excinfo.value)


def test_rmdir_should_rmdir(appmock_client, endpoint, fl, uuid):
    getattr_response = prepare_attr_response(uuid, fuse_messages_pb2.DIR)

    ok = messages_pb2.ServerMessage()
    ok.fuse_response.status.code = common_messages_pb2.Status.ok

    with reply(endpoint, [ok, ok, ok, getattr_response, ok]) as queue:
        fl.rmdir('parentUuid', 'name')
        queue.get()
        queue.get()
        queue.get()
        queue.get()
        client_message = queue.get()

    assert client_message.HasField('fuse_request')
    assert client_message.fuse_request.HasField('file_request')

    file_request = client_message.fuse_request.file_request
    assert file_request.HasField('delete_file')
    assert file_request.context_guid == \
           getattr_response.fuse_response.file_attr.uuid

    with pytest.raises(RuntimeError) as excinfo:
        fl.getattr(uuid)

    assert 'No such file or directory' in str(excinfo.value)


def test_rmdir_should_pass_rmdir_errors(appmock_client, endpoint, fl, uuid):
    getattr_response = prepare_attr_response(uuid, fuse_messages_pb2.DIR)
    getattr_parent_response = \
        prepare_attr_response('parentUuid', fuse_messages_pb2.DIR)
    ok = messages_pb2.ServerMessage()
    ok.fuse_response.status.code = common_messages_pb2.Status.eperm

    with pytest.raises(RuntimeError) as excinfo:
        with reply(endpoint, [ok, ok, ok, getattr_response, ok]):
            fl.rmdir('parentUuid', 'filename')

    assert 'Operation not permitted' in str(excinfo.value)


def test_rename_should_rename_file_with_different_uuid(appmock_client, endpoint, fl, uuid):
    ok = prepare_status_response()
    getattr_response = \
        prepare_attr_response(uuid, fuse_messages_pb2.REG, 1024, 'parentUuid')
    getattr_parent_response = \
        prepare_attr_response('parentUuid', fuse_messages_pb2.DIR)
    getattr_newparent_response = \
        prepare_attr_response('newParentUuid', fuse_messages_pb2.DIR)
    rename_response = prepare_rename_response('newUuid')

    #
    # Prepare first response with 5 files
    #
    repl = prepare_file_children_attr_response('parentUuid', "afiles-", 5)
    repl.is_last = True

    readdir_response = messages_pb2.ServerMessage()
    readdir_response.fuse_response.file_children_attrs.CopyFrom(repl)
    readdir_response.fuse_response.status.code = common_messages_pb2.Status.ok

    with reply(endpoint, [getattr_response,
                          getattr_parent_response,
                          getattr_newparent_response,
                          ok, ok, ok,
                          readdir_response,
                          rename_response]) as queue:
        # Ensure the source file is cached
        fl.getattr(uuid)
        queue.get()
        # Ensure the target directory is cached
        d = fl.opendir('newParentUuid')
        fl.readdir('newParentUuid', 100, 0)
        fl.releasedir('newParentUuid', d)
        queue.get()
        queue.get()
        queue.get()
        queue.get()
        # Move the file to new directory
        fl.rename('parentUuid', 'filename', 'newParentUuid', 'newName')
        queue.get()
        queue.get()
        client_message = queue.get()

    assert not fl.metadata_cache_contains(uuid)
    assert fl.metadata_cache_contains('newUuid')

    assert client_message.HasField('fuse_request')
    assert client_message.fuse_request.HasField('file_request')

    file_request = client_message.fuse_request.file_request
    assert file_request.HasField('rename')

    rename = file_request.rename
    assert rename.target_parent_uuid == b'newParentUuid'
    assert rename.target_name == b'newName'
    assert file_request.context_guid == \
           getattr_response.fuse_response.file_attr.uuid


def test_rename_should_rename_file_with_the_same_uuid(appmock_client, endpoint, fl, uuid):
    ok = prepare_status_response()
    getattr_response = \
        prepare_attr_response(uuid, fuse_messages_pb2.REG, 1024, 'parentUuid')
    getattr_parent_response = \
        prepare_attr_response('parentUuid', fuse_messages_pb2.DIR)
    getattr_newparent_response = \
        prepare_attr_response('newParentUuid', fuse_messages_pb2.DIR)
    rename_response = prepare_rename_response(uuid)

    #
    # Prepare first response with 5 files
    #
    repl = prepare_file_children_attr_response('parentUuid', "afiles-", 5)
    repl.is_last = True

    readdir_response = messages_pb2.ServerMessage()
    readdir_response.fuse_response.file_children_attrs.CopyFrom(repl)
    readdir_response.fuse_response.status.code = common_messages_pb2.Status.ok


    with reply(endpoint, [getattr_response,
                          getattr_parent_response,
                          getattr_newparent_response,
                          ok, ok, ok,
                          readdir_response,
                          rename_response]) as queue:
        # Ensure the source file is cached
        fl.getattr(uuid)
        queue.get()
        # Ensure the target directory is cached
        d = fl.opendir('newParentUuid')
        fl.readdir('newParentUuid', 100, 0)
        fl.releasedir('newParentUuid', d)
        queue.get()
        queue.get()
        queue.get()
        queue.get()
        fl.rename('parentUuid', 'filename', 'newParentUuid', 'newName')
        queue.get()
        queue.get()
        client_message = queue.get()

    assert client_message.HasField('fuse_request')
    assert client_message.fuse_request.HasField('file_request')

    file_request = client_message.fuse_request.file_request
    assert file_request.HasField('rename')

    rename = file_request.rename
    assert rename.target_parent_uuid == b'newParentUuid'
    assert rename.target_name == b'newName'
    assert file_request.context_guid == \
           getattr_response.fuse_response.file_attr.uuid


def test_rename_should_rename_directory(appmock_client, endpoint, fl, uuid):
    ok = prepare_status_response()
    getattr_response = \
        prepare_attr_response(uuid, fuse_messages_pb2.DIR, 1234, 'parentUuid', 'name')
    getattr_parent_response = \
        prepare_attr_response('parentUuid', fuse_messages_pb2.DIR)
    getattr_newparent_response = \
        prepare_attr_response('newParentUuid', fuse_messages_pb2.DIR)
    getattr_newattr_response = \
        prepare_attr_response(uuid, fuse_messages_pb2.DIR, 1234, 'parentUuid', 'name')
    rename_response = prepare_rename_response('newUuid')

    with reply(endpoint, [ok, ok, ok, getattr_response,
                          getattr_parent_response,
                          rename_response,
                          getattr_newattr_response]) as queue:
        fl.rename('parentUuid', 'name', 'newParentUuid', 'newName')
        queue.get()
        queue.get()
        queue.get()
        queue.get()
        queue.get()
        client_message = queue.get()

    assert client_message.HasField('fuse_request')
    assert client_message.fuse_request.HasField('file_request')

    file_request = client_message.fuse_request.file_request
    assert file_request.HasField('rename')

    rename = file_request.rename
    assert rename.target_parent_uuid == b'newParentUuid'
    assert rename.target_name == b'newName'
    assert file_request.context_guid == \
           getattr_response.fuse_response.file_attr.uuid


def test_rename_event_should_ignore_uncached_files(appmock_client, endpoint, fl, uuid):
    evt = prepare_file_renamed_event(
            uuid, uuid, 'newName', 'newParentUuid')

    with send(endpoint, [evt]):
        pass

    assert not fl.metadata_cache_contains(uuid)
    assert not fl.metadata_cache_contains('newParentUuid')


def test_rename_event_should_update_old_file_parent_cache(appmock_client, endpoint, fl):
    parentUuid = 'parentUuid'
    ok = prepare_status_response()
    getattr_reponse = prepare_attr_response(parentUuid, fuse_messages_pb2.DIR)
    getattr_newattr_response = \
        prepare_attr_response('newUuid', fuse_messages_pb2.DIR, 1234, 'parentUuid', 'name')

    #
    # Prepare first response with 3 files
    #
    dir_size = 3
    repl = prepare_file_children_attr_response(parentUuid, "afiles-", dir_size)
    repl.is_last = True

    readdir_response = messages_pb2.ServerMessage()
    readdir_response.fuse_response.file_children_attrs.CopyFrom(repl)
    readdir_response.fuse_response.status.code = common_messages_pb2.Status.ok

    # When adding the first directory entry, the client will make sure that the
    # parent attributes are also cached
    getattr_parent_response = prepare_attr_response(parentUuid, fuse_messages_pb2.DIR)

    children = []
    offset = 0
    chunk_size = 50
    with reply(endpoint, [getattr_reponse,
                          ok, ok, ok,
                          readdir_response]) as queue:
        d = fl.opendir(parentUuid)
        children_chunk = fl.readdir(parentUuid, chunk_size, offset)
        queue.get()
        queue.get()
        queue.get()
        queue.get()
        fl.releasedir(parentUuid, d)
        assert len(children_chunk) == len(['.', '..']) + dir_size

    uuid = repl.child_attrs[0].uuid

    evt = prepare_file_renamed_event(
            uuid, 'newUuid', 'newName', 'newParentUuid')

    with send(endpoint, [evt]):
        wait_until(lambda: not fl.metadata_cache_contains(uuid))

    assert fl.metadata_cache_contains('parentUuid')
    assert not fl.metadata_cache_contains('newUuid')
    assert not fl.metadata_cache_contains('newParentUuid')

    children_chunk = []
    with reply(endpoint, [getattr_newattr_response]) as queue:
        children_chunk = fl.readdir(parentUuid, chunk_size, offset)

    assert len(children_chunk) == len(['.', '..']) + dir_size - 1


def test_rename_event_should_update_new_file_parent_cache(appmock_client, endpoint, fl, uuid):
    parentUuid = 'newParentUuid'
    ok = prepare_status_response()
    getattr_reponse = prepare_attr_response(parentUuid, fuse_messages_pb2.DIR)

    #
    # Prepare first response with 3 files
    #
    dir_size = 3
    repl = prepare_file_children_attr_response(parentUuid, "afiles-", dir_size)
    repl.is_last = True

    readdir_response = messages_pb2.ServerMessage()
    readdir_response.fuse_response.file_children_attrs.CopyFrom(repl)
    readdir_response.fuse_response.status.code = common_messages_pb2.Status.ok

    # When adding the first directory entry, the client will make sure that the
    # parent attributes are also cached
    getattr_parent_response = prepare_attr_response(parentUuid, fuse_messages_pb2.DIR)

    children = []
    offset = 0
    chunk_size = 50
    with reply(endpoint, [getattr_reponse,
                          ok, ok, ok,
                          readdir_response]) as queue:
        d = fl.opendir(parentUuid)
        children_chunk = fl.readdir(parentUuid, chunk_size, offset)
        queue.get()
        queue.get()
        queue.get()
        queue.get()
        fl.releasedir(parentUuid, d)
        assert len(children_chunk) == len(['.', '..']) + dir_size

    evt = prepare_file_renamed_event(
            uuid, 'newUuid', 'afiles-NEW', 'newParentUuid')

    getattr_new_response = \
        prepare_attr_response('newUuid', fuse_messages_pb2.REG, 1234,
                'newParentUuid', 'afiles-NEW')

    with send(endpoint, [evt]):
        pass

    with reply(endpoint, [getattr_new_response]) as queue:
        wait_until(lambda: fl.metadata_cache_contains('newUuid'))
        assert not fl.metadata_cache_contains(uuid)
        assert fl.metadata_cache_contains('newParentUuid')

    children_chunk = fl.readdir(parentUuid, chunk_size, offset)

    assert len(children_chunk) == len(['.', '..']) + dir_size + 1
    assert 'afiles-NEW' in children_chunk


def test_rename_should_update_cache(appmock_client, endpoint, fl, uuid):
    parentUuid = 'parentUuid'
    newParentUuid = 'newParentUuid'
    dir_size = 3
    offset = 0
    chunk_size = 100

    ok = prepare_status_response()
    #
    # Prepare first response with 3 files
    #
    repl = prepare_file_children_attr_response(parentUuid, "afiles-", dir_size)
    repl.is_last = True

    readdir_response = messages_pb2.ServerMessage()
    readdir_response.fuse_response.file_children_attrs.CopyFrom(repl)
    readdir_response.fuse_response.status.code = common_messages_pb2.Status.ok

    # When adding the first directory entry, the client will make sure that the
    # parent attributes are also cached
    getattr_parent_response = prepare_attr_response(parentUuid, fuse_messages_pb2.DIR)

    #
    # Prepare first response with 3 files
    #
    repl_new = prepare_file_children_attr_response(newParentUuid, "bfiles-", dir_size)
    repl_new.is_last = True

    readdir_new_response = messages_pb2.ServerMessage()
    readdir_new_response.fuse_response.file_children_attrs.CopyFrom(repl_new)
    readdir_new_response.fuse_response.status.code = common_messages_pb2.Status.ok

    # When adding the first directory entry, the client will make sure that the
    # parent attributes are also cached
    getattr_newparent_response = prepare_attr_response(newParentUuid, fuse_messages_pb2.DIR)

    rename_response = prepare_rename_response('newUuid')

    with reply(endpoint, [getattr_parent_response,
                          ok, ok, ok,
                          readdir_response,
                          getattr_newparent_response,
                          ok, ok, ok,
                          readdir_new_response,
                          rename_response]) as queue:
        # Ensure the source directory is cached
        d = fl.opendir(parentUuid)
        fl.readdir(parentUuid, chunk_size, offset)
        fl.releasedir(parentUuid, d)
        # Ensure the target directory is cached
        d = fl.opendir(newParentUuid)
        fl.readdir(newParentUuid, chunk_size, offset)
        fl.releasedir(newParentUuid, d)
        # Rename the file
        fl.rename(parentUuid, 'afiles-0', newParentUuid, 'afiles-NEW')

    assert fl.metadata_cache_contains('newUuid')

    children_chunk = fl.readdir(parentUuid, chunk_size, offset)

    assert len(children_chunk) == len(['.', '..']) + dir_size - 1
    assert 'afiles-0' not in children_chunk

    children_chunk = fl.readdir(newParentUuid, chunk_size, offset)

    assert len(children_chunk) == len(['.', '..']) + dir_size + 1
    assert 'afiles-NEW' in children_chunk


def test_rename_should_pass_rename_errors(appmock_client, endpoint, fl, uuid):
    ok = prepare_status_response()
    getattr_response = prepare_attr_response(uuid, fuse_messages_pb2.DIR)
    response = messages_pb2.ServerMessage()
    response.fuse_response.status.code = common_messages_pb2.Status.eperm

    with pytest.raises(RuntimeError) as excinfo:
        with reply(endpoint, [ok, ok, getattr_response,
                              response]):
            fl.rename('parentUuid', 'name', 'newParentUuid', 'newName')

    assert 'Operation not permitted' in str(excinfo.value)


def test_chmod_should_change_mode(appmock_client, endpoint, fl, uuid):
    ok = prepare_status_response()
    getattr_parent_response = \
        prepare_attr_response('parentUuid', fuse_messages_pb2.DIR)
    getattr_response = \
        prepare_attr_response(uuid, fuse_messages_pb2.REG, 1024, 'parentUuid')

    with reply(endpoint, [ok,
                          getattr_parent_response,
                          ok,
                          getattr_response]) as queue:
        fl.chmod(uuid, 0o123)
        client_message = queue.get()

    assert client_message.HasField('fuse_request')
    assert client_message.fuse_request.HasField('file_request')

    file_request = client_message.fuse_request.file_request
    assert file_request.HasField('change_mode')

    change_mode = file_request.change_mode
    assert change_mode.mode == 0o123
    assert file_request.context_guid == \
           getattr_response.fuse_response.file_attr.uuid


def test_chmod_should_change_cached_mode(appmock_client, endpoint, fl, uuid, parentUuid):
    getattr_response = prepare_attr_response(uuid, fuse_messages_pb2.REG, 1, parentUuid)
    getattr_parent_response = prepare_attr_response(parentUuid, fuse_messages_pb2.DIR)

    with reply(endpoint, [getattr_response,
                          getattr_parent_response]):
        stat = fl.getattr(uuid)

    assert stat.mode == getattr_response.fuse_response.file_attr.mode | \
                        fslogic.regularMode()
    appmock_client.reset_tcp_history()

    response = messages_pb2.ServerMessage()
    response.fuse_response.status.code = common_messages_pb2.Status.ok
    with reply(endpoint, [response, response]):
        fl.chmod(uuid, 0o356)

    stat = fl.getattr(uuid)

    assert stat.mode == 0o356 | fslogic.regularMode()


def test_chmod_should_pass_chmod_errors(appmock_client, endpoint, fl, uuid):
    response = messages_pb2.ServerMessage()
    response.fuse_response.status.code = common_messages_pb2.Status.enoent

    with pytest.raises(RuntimeError) as excinfo:
        with reply(endpoint, response):
            fl.chmod(uuid, 0o312)

    assert 'No such file or directory' in str(excinfo.value)


def test_utime_should_update_times(appmock_client, endpoint, fl, uuid, stat):
    ok = messages_pb2.ServerMessage()
    ok.fuse_response.status.code = common_messages_pb2.Status.ok

    with reply(endpoint, ok) as queue:
        fl.utime(uuid)
        client_message = queue.get()

    assert client_message.HasField('fuse_request')
    assert client_message.fuse_request.HasField('file_request')

    file_request = client_message.fuse_request.file_request
    assert file_request.HasField('update_times')

    update_times = file_request.update_times
    assert update_times.atime == update_times.mtime
    assert update_times.atime == update_times.ctime
    assert update_times.atime <= time.time()
    assert file_request.context_guid == uuid.encode('utf-8')


def test_utime_should_change_cached_times(appmock_client, endpoint, fl, uuid, parentUuid):
    getattr_response = \
        prepare_attr_response(uuid, fuse_messages_pb2.REG, 1, parentUuid)
    getattr_parent_response = \
        prepare_attr_response(parentUuid, fuse_messages_pb2.DIR)

    with reply(endpoint, [getattr_response,
                          getattr_parent_response]):
        stat = fl.getattr(uuid)

    assert stat.atime == getattr_response.fuse_response.file_attr.atime
    assert stat.mtime == getattr_response.fuse_response.file_attr.mtime
    appmock_client.reset_tcp_history()

    response = messages_pb2.ServerMessage()
    response.fuse_response.status.code = common_messages_pb2.Status.ok
    with reply(endpoint, response):
        fl.utime(uuid)

    stat = fslogic.Stat()
    fl.getattr(uuid)

    assert stat.atime != getattr_response.fuse_response.file_attr.atime
    assert stat.mtime != getattr_response.fuse_response.file_attr.mtime


def test_utime_should_update_times_with_buf(appmock_client, endpoint, fl, uuid, stat):
    response = messages_pb2.ServerMessage()
    response.fuse_response.status.code = common_messages_pb2.Status.ok

    ubuf = fslogic.Ubuf()
    ubuf.actime = 54321
    ubuf.modtime = 12345

    with reply(endpoint, response) as queue:
        fl.utime_buf(uuid, ubuf)
        client_message = queue.get()

    assert client_message.HasField('fuse_request')
    assert client_message.fuse_request.HasField('file_request')

    file_request = client_message.fuse_request.file_request
    assert file_request.HasField('update_times')

    update_times = file_request.update_times
    assert update_times.atime == ubuf.actime
    assert update_times.mtime == ubuf.modtime
    assert file_request.context_guid == uuid.encode('utf-8')


def test_utime_should_pass_utime_errors(appmock_client, endpoint, fl, uuid, stat):
    response = messages_pb2.ServerMessage()
    response.fuse_response.status.code = common_messages_pb2.Status.eperm

    with pytest.raises(RuntimeError) as excinfo:
        with reply(endpoint, response):
            fl.utime(uuid)

    assert 'Operation not permitted' in str(excinfo.value)

    ubuf = fslogic.Ubuf()
    with pytest.raises(RuntimeError) as excinfo:
        with reply(endpoint, response):
            fl.utime_buf(uuid, ubuf)

    assert 'Operation not permitted' in str(excinfo.value)


def test_readdir_should_read_dir(appmock_client, endpoint, fl, stat):
    uuid = 'parentUuid'

    ok = prepare_status_response()
    getattr_response = prepare_attr_response(uuid, fuse_messages_pb2.DIR)

    #
    # Prepare first response with 5 files
    #
    repl1 = prepare_file_children_attr_response(uuid, "afiles-", 5)
    repl1.is_last = False

    response1 = messages_pb2.ServerMessage()
    response1.fuse_response.file_children_attrs.CopyFrom(repl1)
    response1.fuse_response.status.code = common_messages_pb2.Status.ok

    #
    # Prepare second response with another 5 file
    #
    repl2 = prepare_file_children_attr_response(uuid, "bfiles-", 5)
    repl2.is_last = True

    response2 = messages_pb2.ServerMessage()
    response2.fuse_response.file_children_attrs.CopyFrom(repl2)
    response2.fuse_response.status.code = common_messages_pb2.Status.ok

    children = []
    offset = 0
    chunk_size = 50
    with reply(endpoint, [getattr_response,
                          ok, ok, ok,
                          response1,
                          response2]) as queue:
        d = fl.opendir(uuid)
        children_chunk = fl.readdir(uuid, chunk_size, offset)
        queue.get()
        queue.get()
        queue.get()
        queue.get()
        fl.releasedir(uuid, d)
        assert len(children_chunk) == 12

    time.sleep(2)

    #
    # After the last request the value should be available
    # from readdir cache, without any communication with provider
    #
    for i in range(3):
        with reply(endpoint, []) as queue:
            d = fl.opendir(uuid)
            children_chunk = fl.readdir(uuid, 5, 0)
            fl.releasedir(uuid, d)
            assert len(children_chunk) == 5
        time.sleep(1)


def test_readdir_should_skip_incomplete_replicas(appmock_client, endpoint, fl_onlyfullreplicas):
    uuid = 'parentUuid'

    ok = prepare_status_response()
    getattr_reponse = prepare_attr_response(uuid, fuse_messages_pb2.DIR)

    #
    # Prepare first response with 5 files
    #
    repl1 = prepare_file_children_attr_response(uuid, "afiles-", 5)
    for f in repl1.child_attrs:
        f.fully_replicated = True
    repl1.is_last = False

    response1 = messages_pb2.ServerMessage()
    response1.fuse_response.file_children_attrs.CopyFrom(repl1)
    response1.fuse_response.status.code = common_messages_pb2.Status.ok

    #
    # Prepare second response with another 5 file
    #
    repl2 = prepare_file_children_attr_response(uuid, "bfiles-", 5)
    for f in repl2.child_attrs:
        f.fully_replicated = False
    repl2.is_last = True

    response2 = messages_pb2.ServerMessage()
    response2.fuse_response.file_children_attrs.CopyFrom(repl2)
    response2.fuse_response.status.code = common_messages_pb2.Status.ok

    children = []
    offset = 0
    chunk_size = 50
    with reply(endpoint, [getattr_reponse,
                          ok, ok, ok, ok,
                          response1,
                          response2]) as queue:
        d = fl_onlyfullreplicas.opendir(uuid)
        children_chunk = fl_onlyfullreplicas.readdir(uuid, chunk_size, offset)
        _ = queue.get()
        fl_onlyfullreplicas.releasedir(uuid, d)
        assert len(children_chunk) == 5+2

    time.sleep(2)

    #
    # After the last request the value should be available
    # from readdir cache, without any communication with provider
    #
    for i in range(3):
        with reply(endpoint, []) as queue:
            d = fl_onlyfullreplicas.opendir(uuid)
            children_chunk = fl_onlyfullreplicas.readdir(uuid, 5, 0)
            fl_onlyfullreplicas.releasedir(uuid, d)
            assert len(children_chunk) == 5
        time.sleep(1)


def test_readdir_should_handle_fileattrchanged_event(appmock_client, endpoint, fl, parentUuid, stat):
    ok = prepare_status_response()
    getattr_reponse = prepare_attr_response(parentUuid, fuse_messages_pb2.DIR)

    #
    # Prepare first response with 3 files
    #
    dir_size = 3
    repl = prepare_file_children_attr_response(parentUuid, "afiles-", dir_size)
    repl.is_last = True

    readdir_response = messages_pb2.ServerMessage()
    readdir_response.fuse_response.file_children_attrs.CopyFrom(repl)
    readdir_response.fuse_response.status.code = common_messages_pb2.Status.ok

    # When adding the first directory entry, the client will make sure that the
    # parent attributes are also cached
    getattr_parent_response = prepare_attr_response(parentUuid, fuse_messages_pb2.DIR)

    children = []
    offset = 0
    chunk_size = 50
    with reply(endpoint, [getattr_reponse,
                          ok, ok, ok,
                          readdir_response]) as queue:
        d = fl.opendir(parentUuid)
        children_chunk = fl.readdir(parentUuid, chunk_size, offset)
        _ = queue.get()
        fl.releasedir(parentUuid, d)
        assert len(children_chunk) == len(['.', '..']) + dir_size

    #
    # After readdir is complete, file attributes are available from cache
    #
    file_uuid = repl.child_attrs[0].uuid
    attr = fl.getattr(file_uuid)

    evt = prepare_file_attr_changed_event(
            file_uuid, fuse_messages_pb2.REG, 12345, parentUuid)
    with send(endpoint, [evt]):
        pass

    time.sleep(1)

    # After the Oneprovider sends FileAttrChanged event, the file should be updated in
    # the cache
    attr = fl.getattr(file_uuid)

    assert attr.size == 12345


def test_readdir_should_return_unique_entries(endpoint, fl, stat):
    uuid = 'parentUuid'

    ok = prepare_status_response()
    getattr_response = prepare_attr_response(uuid, fuse_messages_pb2.DIR)

    #
    # Prepare first response with 5 files
    #
    repl1 = prepare_file_children_attr_response(uuid, "afiles-", 5)
    repl1.is_last = False

    response1 = messages_pb2.ServerMessage()
    response1.fuse_response.file_children_attrs.CopyFrom(repl1)
    response1.fuse_response.status.code = common_messages_pb2.Status.ok

    #
    # Prepare second response with the same 5 files
    #
    repl2 = prepare_file_children_attr_response(uuid, "afiles-", 5)
    repl2.is_last = True

    response2 = messages_pb2.ServerMessage()
    response2.fuse_response.file_children_attrs.CopyFrom(repl2)
    response2.fuse_response.status.code = common_messages_pb2.Status.ok

    children = []
    offset = 0
    chunk_size = 50
    with reply(endpoint, [getattr_response,
                          ok, ok, ok,
                          response1,
                          response2]) as queue:
        d = fl.opendir(uuid)
        children_chunk = fl.readdir(uuid, chunk_size, offset)
        _ = queue.get()
        fl.releasedir(uuid, d)
        children.extend(children_chunk)

    assert len(children) == 5 + 2


def test_readdir_should_pass_readdir_errors(appmock_client, endpoint, fl, stat):
    uuid = 'parentUuid'

    ok = prepare_status_response()
    getattr_reponse = prepare_attr_response(uuid, fuse_messages_pb2.DIR)

    response = messages_pb2.ServerMessage()
    response.fuse_response.status.code = common_messages_pb2.Status.eperm

    with pytest.raises(RuntimeError) as excinfo:
        with reply(endpoint, [getattr_reponse,
                              ok, ok, ok, response]):
            d = fl.opendir(uuid)
            fl.readdir(uuid, 1024, 0)
            fl.releasedir(uuid, d)

    assert 'Operation not permitted' in str(excinfo.value)


def test_readdir_should_not_get_stuck_on_errors(appmock_client, endpoint, fl, stat):
    uuid = 'parentUuid'

    ok = prepare_status_response()
    getattr_response = prepare_attr_response(uuid, fuse_messages_pb2.DIR)

    response0 = messages_pb2.ServerMessage()
    response0.fuse_response.status.code = common_messages_pb2.Status.eperm

    with pytest.raises(RuntimeError) as excinfo:
        with reply(endpoint, [getattr_response,
                              ok, ok, ok,
                              response0]):
            d = fl.opendir(uuid)
            fl.readdir(uuid, 1024, 0)
            fl.releasedir(uuid, d)

    assert 'Operation not permitted' in str(excinfo.value)

    #
    # Prepare first response with 5 files
    #
    repl1 = prepare_file_children_attr_response(uuid, "afiles-", 5)
    repl1.is_last = False

    response1 = messages_pb2.ServerMessage()
    response1.fuse_response.file_children_attrs.CopyFrom(repl1)
    response1.fuse_response.status.code = common_messages_pb2.Status.ok

    #
    # Prepare second response with another 5 file
    #
    repl2 = prepare_file_children_attr_response(uuid, "bfiles-", 5)
    repl2.is_last = True

    response2 = messages_pb2.ServerMessage()
    response2.fuse_response.file_children_attrs.CopyFrom(repl2)
    response2.fuse_response.status.code = common_messages_pb2.Status.ok

    children = []
    offset = 0
    chunk_size = 50
    with reply(endpoint, [response1,
                          response2]) as queue:
        d = fl.opendir(uuid)
        children_chunk = fl.readdir(uuid, chunk_size, offset)
        _ = queue.get()
        fl.releasedir(uuid, d)
        assert len(children_chunk) == 12



def test_metadatacache_should_ignore_changes_on_deleted_files(appmock_client, endpoint, fl):
    ok = prepare_status_response()
    getattr_response = prepare_attr_response('parentUuid', fuse_messages_pb2.DIR)

    #
    # Prepare readdir response with 1 file
    #
    repl1 = prepare_file_children_attr_response('parentUuid', "afiles-", 1)
    repl1.is_last = True

    response1 = messages_pb2.ServerMessage()
    response1.fuse_response.file_children_attrs.CopyFrom(repl1)
    response1.fuse_response.status.code = common_messages_pb2.Status.ok

    children = []
    offset = 0
    chunk_size = 50
    with reply(endpoint, [getattr_response,
                          ok, ok, ok,
                          response1]) as queue:
        d = fl.opendir('parentUuid')
        children_chunk = fl.readdir('parentUuid', chunk_size, offset)
        _ = queue.get()
        fl.releasedir('parentUuid', d)
        children.extend(children_chunk)

    assert len(children) == 1+2

    time.sleep(1)

    assert fl.metadata_cache_size() == 1+1

    afiles_0_uuid = repl1.child_attrs[0].uuid

    fl.getattr(afiles_0_uuid)

    #
    # Remove file 'afiles-0'
    #
    ok = messages_pb2.ServerMessage()
    ok.fuse_response.status.code = common_messages_pb2.Status.ok
    with reply(endpoint, [ok]) as queue:
        fl.unlink('parentUuid', 'afiles-0')

    time.sleep(1)

    evt = prepare_file_attr_changed_event(
        afiles_0_uuid, fuse_messages_pb2.REG, None, 'parentUuid', 0o655)

    with send(endpoint, [evt]):
        pass

    time.sleep(1)

    with pytest.raises(RuntimeError) as excinfo:
        fl.getattr(afiles_0_uuid)

    assert 'No such file or directory' in str(excinfo.value)


def test_metadatacache_should_ignore_changes_on_deleted_directories(appmock_client, endpoint, fl):
    ok = prepare_status_response()
    getattr_response = prepare_attr_response(
        'parentUuid', fuse_messages_pb2.DIR, None, 'parentParentUuid', 'dir1')
    getattr_parent_response = prepare_attr_response(
        'parentParentUuid', fuse_messages_pb2.DIR, None)

    #
    # Prepare readdir response with 1 file
    #
    repl1 = prepare_file_children_attr_response('parentUuid', "afiles-", 1)
    repl1.is_last = True

    response1 = messages_pb2.ServerMessage()
    response1.fuse_response.file_children_attrs.CopyFrom(repl1)
    response1.fuse_response.status.code = common_messages_pb2.Status.ok

    children = []
    offset = 0
    chunk_size = 50
    with reply(endpoint, [getattr_response,
                          getattr_parent_response,
                          ok, ok, ok,
                          response1]) as queue:
        d = fl.opendir('parentUuid')
        children_chunk = fl.readdir('parentUuid', chunk_size, offset)
        queue.get()
        fl.releasedir('parentUuid', d)
        children.extend(children_chunk)

    assert len(children) == 1+2

    time.sleep(1)

    assert fl.metadata_cache_size() == 2+1

    #
    # Remove file 'afiles-0'
    #
    with reply(endpoint, [ok, ok, ok, ok]) as queue:
        fl.unlink('parentParentUuid', 'dir1')

    evt = prepare_file_attr_changed_event(
        'parentUuid', fuse_messages_pb2.DIR, 0, 'parentParentUuid')

    with send(endpoint, [evt]):
        pass

    time.sleep(1)

    with pytest.raises(RuntimeError) as excinfo:
        fl.getattr('parentUuid')

    assert 'No such file or directory' in str(excinfo.value)


def test_metadatacache_should_keep_open_file_metadata(appmock_client, endpoint, fl):
    parent = 'parentUuid'
    name = 'a.txt'
    uuid1 = 'uuid1'
    uuid2 = 'uuid2'
    size = 1024
    blocks=[(0, 10)]
    handle_id = 'handle_id'

    ok = messages_pb2.ServerMessage()
    ok.fuse_response.status.code = common_messages_pb2.Status.ok


    # Create a file and open it, then delete while opened, and the perform a read
    # and release the file
    attr_parent_response = prepare_attr_response(parent, fuse_messages_pb2.DIR)
    attr_response = prepare_attr_response(uuid1, fuse_messages_pb2.REG,
                                          size, parent, name)
    location_response = prepare_location_response(uuid1, blocks)
    open_response = prepare_open_response(handle_id)

    with reply(endpoint, [attr_response,
                          location_response,
                          ok, ok, ok, ok,
                          attr_parent_response,
                          open_response]):
        fh = fl.open(uuid1, 0)
        assert fh >= 0


    assert fl.metadata_cache_contains(uuid1)
    assert fl.metadata_cache_contains(parent)

    with reply(endpoint, [ok, ok, ok, ok]) as queue:
        fl.unlink(parent, name)

    assert not fl.metadata_cache_contains(uuid1)

    assert 5 == len(fl.read(uuid1, fh, 0, 5))

    do_release(endpoint, fl, uuid1, fh)

    # Repeat the same steps again with a different file with the same name
    # in the same directory
    attr_response = prepare_attr_response(uuid2, fuse_messages_pb2.REG,
                                          size, parent, name)
    location_response = prepare_location_response(uuid2, blocks)
    open_response = prepare_open_response(handle_id)

    with reply(endpoint, [attr_response,
                          location_response,
                          ok, ok, ok, ok,
                          open_response]):
        fh = fl.open(uuid2, 0)
        assert fh >= 0

    with reply(endpoint, [ok]) as queue:
        fl.unlink(parent, name)

    assert 5 == len(fl.read(uuid2, fh, 0, 5))

    do_release(endpoint, fl, uuid2, fh)


def test_metadatacache_should_drop_expired_directories(appmock_client, endpoint, fl_dircache):
    ok = messages_pb2.ServerMessage()
    ok.fuse_response.status.code = common_messages_pb2.Status.ok
    getattr_response = prepare_attr_response('parentUuid', fuse_messages_pb2.DIR)

    #
    # Prepare readdir response with 10 files
    #
    repl1 = prepare_file_children_attr_response('parentUuid', "afiles-", 10)
    repl1.is_last = True

    response1 = messages_pb2.ServerMessage()
    response1.fuse_response.file_children_attrs.CopyFrom(repl1)
    response1.fuse_response.status.code = common_messages_pb2.Status.ok

    children = []
    offset = 0
    chunk_size = 50
    with reply(endpoint, [getattr_response,
                          ok, ok, ok, response1]) as queue:
        d = fl_dircache.opendir('parentUuid')
        children_chunk = fl_dircache.readdir('parentUuid', chunk_size, offset)
        _ = queue.get()
        fl_dircache.releasedir('parentUuid', d)
        children.extend(children_chunk)

    assert len(children) == 10+2

    time.sleep(1)

    assert fl_dircache.metadata_cache_size() == 10 + 1

    # Wait past directory cache expiry which is 3 seconds
    time.sleep(5)

    assert fl_dircache.metadata_cache_size() == 1


def test_metadatacache_should_drop_expired_directories_and_keep_parent_entries(appmock_client, endpoint, fl_dircache):
    ok = messages_pb2.ServerMessage()
    ok.fuse_response.status.code = common_messages_pb2.Status.ok
    getattr_response = prepare_attr_response('parentUuid', fuse_messages_pb2.DIR)

    #
    # Prepare readdir response with 3 directories and 3 files in each directory
    #
    repl1 = prepare_file_children_attr_response('parentUuid', "parents-", 3)
    repl1.is_last = True

    parent_uuid_0 = repl1.child_attrs[0].uuid
    repl1.child_attrs[0].type = fuse_messages_pb2.DIR
    parent_uuid_1 = repl1.child_attrs[1].uuid
    repl1.child_attrs[1].type = fuse_messages_pb2.DIR
    parent_uuid_2 = repl1.child_attrs[2].uuid
    repl1.child_attrs[2].type = fuse_messages_pb2.DIR

    response1 = messages_pb2.ServerMessage()
    response1.fuse_response.file_children_attrs.CopyFrom(repl1)
    response1.fuse_response.status.code = common_messages_pb2.Status.ok

    repl2 = prepare_file_children_attr_response(parent_uuid_0, "children-", 3)
    repl2.is_last = True

    child_uuid_0 = repl2.child_attrs[0].uuid
    child_uuid_1 = repl2.child_attrs[1].uuid
    child_uuid_2 = repl2.child_attrs[2].uuid

    response2 = messages_pb2.ServerMessage()
    response2.fuse_response.file_children_attrs.CopyFrom(repl2)
    response2.fuse_response.status.code = common_messages_pb2.Status.ok


    #
    # First list the top directory
    #
    children = []
    offset = 0
    chunk_size = 50
    with reply(endpoint, [getattr_response,
                          ok, ok, ok, response1]) as queue:
        d = fl_dircache.opendir('parentUuid')
        children_chunk = fl_dircache.readdir('parentUuid', chunk_size, offset)
        _ = queue.get()
        fl_dircache.releasedir('parentUuid', d)
        children.extend(children_chunk)

    assert children.sort() == ['.', '..', 'parents-0', 'parents-1', 'parents-2'].sort()

    #
    # Now list parent-0 directory
    #
    children = []
    offset = 0
    chunk_size = 50
    with reply(endpoint, [ok, ok, ok, response2]) as queue:
        d = fl_dircache.opendir(parent_uuid_0)
        children_chunk = fl_dircache.readdir(parent_uuid_0, chunk_size, offset)
        _ = queue.get()
        fl_dircache.releasedir(parent_uuid_0, d)
        children.extend(children_chunk)

    assert fl_dircache.metadata_cache_contains(child_uuid_0)
    assert fl_dircache.metadata_cache_contains(child_uuid_1)
    assert fl_dircache.metadata_cache_contains(child_uuid_2)

    # Wait so that the contents of 'parents-0' are invalidated but keep
    # the top directory active
    time.sleep(2)
    attr = fl_dircache.getattr(parent_uuid_1)

    time.sleep(2)
    attr = fl_dircache.getattr(parent_uuid_1)

    time.sleep(2)
    attr = fl_dircache.getattr(parent_uuid_1)

    assert not fl_dircache.metadata_cache_contains(child_uuid_0)
    assert not fl_dircache.metadata_cache_contains(child_uuid_1)
    assert not fl_dircache.metadata_cache_contains(child_uuid_2)

    assert fl_dircache.metadata_cache_contains(parent_uuid_2)
    assert fl_dircache.metadata_cache_contains(parent_uuid_1)
    assert fl_dircache.metadata_cache_contains(parent_uuid_0)


def test_metadatacache_should_prune_when_size_exceeded(appmock_client, endpoint, fl_dircache):
    ok = prepare_status_response()
    getattr_response = prepare_attr_response('parentUuid', fuse_messages_pb2.DIR)

    #
    # Prepare readdir response with 20 files
    #
    repl1 = prepare_file_children_attr_response('parentUuid', "afiles-", 20)
    repl1.is_last = True

    response1 = messages_pb2.ServerMessage()
    response1.fuse_response.file_children_attrs.CopyFrom(repl1)
    response1.fuse_response.status.code = common_messages_pb2.Status.ok

    children = []
    offset = 0
    chunk_size = 50
    with reply(endpoint, [getattr_response,
                          ok, ok, ok, response1]) as queue:
        d = fl_dircache.opendir('parentUuid')
        children_chunk = fl_dircache.readdir('parentUuid', chunk_size, offset)
        _ = queue.get()
        fl_dircache.releasedir('parentUuid', d)
        children.extend(children_chunk)

    assert len(children) == 20+2

    time.sleep(1)

    assert fl_dircache.metadata_cache_size() == 21

    getattr_response2 = prepare_attr_response('parentUuid2', fuse_messages_pb2.DIR)

    repl2 = prepare_file_children_attr_response('parentUuid2', "bfiles-", 10)
    repl2.is_last = True

    response2 = messages_pb2.ServerMessage()
    response2.fuse_response.file_children_attrs.CopyFrom(repl2)
    response2.fuse_response.status.code = common_messages_pb2.Status.ok

    children = []
    offset = 0
    chunk_size = 50
    with reply(endpoint, [getattr_response2,
                          ok, ok, ok, response2]) as queue:
        d = fl_dircache.opendir('parentUuid2')
        children_chunk = fl_dircache.readdir('parentUuid2', chunk_size, offset)
        _ = queue.get()
        fl_dircache.releasedir('parentUuid2', d)
        children.extend(children_chunk)

    assert len(children) == 10+2

    time.sleep(1)

    assert not fl_dircache.metadata_cache_contains(repl1.child_attrs[0].uuid)
    assert fl_dircache.metadata_cache_contains(repl2.child_attrs[0].uuid)

    assert fl_dircache.metadata_cache_size() == 11


def test_link_should_create_hard_link(appmock_client, endpoint, fl, uuid, parentUuid):
    name = random_str()
    attr_response = prepare_attr_response(
                    uuid, fuse_messages_pb2.LNK, size=0,
                    parent_uuid=parentUuid, name=name)

    with reply(endpoint, attr_response) as queue:
        fl.link(uuid, parentUuid, name)
        client_message = queue.get()

    assert client_message.HasField('fuse_request')
    assert client_message.fuse_request.HasField('file_request')

    file_request = client_message.fuse_request.file_request
    assert file_request.HasField('make_link')


def test_symlink_should_create_symbolic_link(appmock_client, endpoint, fl, uuid, parentUuid):
    name = random_str()
    link = random_str()
    attr_response = prepare_attr_response(
                    uuid, fuse_messages_pb2.SYMLNK, size=len(link),
                    parent_uuid=parentUuid, name=name)

    with reply(endpoint, attr_response) as queue:
        fl.symlink(parentUuid, name, link)
        client_message = queue.get()

    assert client_message.HasField('fuse_request')
    assert client_message.fuse_request.HasField('file_request')

    file_request = client_message.fuse_request.file_request
    assert file_request.HasField('make_symlink')


def test_readlink_should_read_symbolic_link(appmock_client, endpoint, fl, uuid):
    link = random_str()
    readlink_response = prepare_readlink_response(uuid, link)

    with reply(endpoint, readlink_response) as queue:
        link_result = fl.readlink(uuid)
        client_message = queue.get()

    assert client_message.HasField('fuse_request')
    assert client_message.fuse_request.HasField('file_request')

    file_request = client_message.fuse_request.file_request
    assert file_request.HasField('read_symlink')

    assert(link == link_result)


def test_mknod_should_create_multiple_files(appmock_client, endpoint, fl, uuid, parentUuid, parentStat):
    getattr_responses = []
    for i in range(0,100):
        getattr_responses.append(prepare_attr_response(
                    uuid+'_'+str(i), fuse_messages_pb2.REG, size=0,
                    parent_uuid=parentUuid, name='filename_'+str(i)))

    with reply(endpoint, getattr_responses) as queue:
        for i in range(0,100):
            fl.mknod(parentUuid, 'filename_'+str(i), 0o664 | S_IFREG)
            client_message = queue.get()

    assert client_message.HasField('fuse_request')
    assert client_message.fuse_request.HasField('file_request')

    file_request = client_message.fuse_request.file_request
    assert file_request.HasField('make_file')

    make_file = file_request.make_file
    assert make_file.name == b'filename_99'
    assert make_file.mode == 0o664
    assert file_request.context_guid == parentUuid.encode('utf-8')


def test_mknod_should_make_new_location(appmock_client, endpoint, fl, uuid, parentUuid, parentStat):
    getattr_response = prepare_attr_response(uuid, fuse_messages_pb2.REG, 10, parentUuid)

    with reply(endpoint, [getattr_response]) as queue:
        fl.mknod(parentUuid, 'childName', 0o762 | S_IFREG)
        client_message = queue.get()

    assert client_message.HasField('fuse_request')
    assert client_message.fuse_request.HasField('file_request')

    file_request = client_message.fuse_request.file_request
    assert file_request.HasField('make_file')

    make_file = file_request.make_file
    assert make_file.name == b'childName'
    assert make_file.mode == 0o762
    assert file_request.context_guid == parentUuid.encode('utf-8')


def test_mknod_should_pass_location_errors(appmock_client, endpoint, fl, parentUuid, parentStat):
    response = messages_pb2.ServerMessage()
    response.fuse_response.status.code = common_messages_pb2.Status.eperm

    with pytest.raises(RuntimeError) as excinfo:
        with reply(endpoint, response):
            fl.mknod(parentUuid, 'childName', 0o123)

    assert 'Operation not permitted' in str(excinfo.value)


def test_mknod_should_throw_on_unsupported_file_type(endpoint, fl, parentUuid, parentStat):
    response = messages_pb2.ServerMessage()
    response.fuse_response.status.code = common_messages_pb2.Status.eperm

    with pytest.raises(RuntimeError) as excinfo:
        fl.mknod(parentUuid, 'childName', 0o664 | S_IFSOCK)

    assert 'Operation not supported' in str(excinfo.value)

    with pytest.raises(RuntimeError) as excinfo:
        fl.mknod(parentUuid, 'childName', 0o664 | S_IFBLK)

    assert 'Operation not supported' in str(excinfo.value)

    with pytest.raises(RuntimeError) as excinfo:
        fl.mknod(parentUuid, 'childName', 0o664 | S_IFDIR)

    assert 'Operation not supported' in str(excinfo.value)

    with pytest.raises(RuntimeError) as excinfo:
        fl.mknod(parentUuid, 'childName', 0o664 | S_IFCHR)

    assert 'Operation not supported' in str(excinfo.value)

    with pytest.raises(RuntimeError) as excinfo:
        fl.mknod(parentUuid, 'childName', 0o664 | S_IFIFO)

    assert 'Operation not supported' in str(excinfo.value)


def test_read_should_read_range(appmock_client, endpoint, fl, uuid):
    ok = prepare_status_response()

    fh = do_open(endpoint, fl, uuid, blocks=[(0, 10)])

    assert 5 == len(fl.read(uuid, fh, 0, 5))

    do_release(endpoint, fl, uuid, fh)


def test_read_should_fetch_file_location_after_closing_file(appmock_client, endpoint, fl, uuid):
    blocks = [(0,10)]
    fh = do_open(endpoint, fl, uuid, blocks=blocks)

    assert 5 == len(fl.read(uuid, fh, 0, 5))

    do_release(endpoint, fl, uuid, fh)

    fh = do_open_cached(endpoint, fl, uuid, blocks=blocks)

    assert 5 == len(fl.read(uuid, fh, 0, 5))

    do_release(endpoint, fl, uuid, fh)


def test_read_should_read_zero_on_eof(appmock_client, endpoint, fl, uuid):
    fh = do_open(endpoint, fl, uuid, size=10, blocks=[(0, 10)])

    assert 10 == len(fl.read(uuid, fh, 0, 12))
    assert 0 == len(fl.read(uuid, fh, 10, 2))

    do_release(endpoint, fl, uuid, fh)


def test_read_should_pass_helper_errors(appmock_client, endpoint, fl, uuid):
    fh = do_open(endpoint, fl, uuid, size=10, blocks=[(0, 10)])

    with pytest.raises(RuntimeError) as excinfo:
        fl.failHelper()
        fl.read(uuid, fh, 0, 10)

    assert 'Owner died' in str(excinfo.value)

    do_release(endpoint, fl, uuid, fh)


def test_write_should_write(appmock_client, endpoint, fl, uuid):
    fh = do_open(endpoint, fl, uuid, size=10, blocks=[(0, 10)])

    assert 5 == fl.write(uuid, fh, 0, 5)

    do_release(endpoint, fl, uuid, fh)


def test_write_should_change_file_size(appmock_client, endpoint, fl, uuid):
    fh = do_open(endpoint, fl, uuid, size=5, blocks=[(0, 5)])
    assert 20 == fl.write(uuid, fh, 10, 20)

    stat = fl.getattr(uuid)
    assert 30 == stat.size

    do_release(endpoint, fl, uuid, fh)


def test_write_should_pass_helper_errors(appmock_client, endpoint, fl, uuid):
    fh = do_open(endpoint, fl, uuid, size=10, blocks=[(0, 10)])

    with pytest.raises(RuntimeError) as excinfo:
        fl.failHelper()
        fl.write(uuid, fh, 0, 10)

    assert 'Owner died' in str(excinfo.value)

    do_release(endpoint, fl, uuid, fh)


def test_truncate_should_truncate(appmock_client, endpoint, fl, uuid, stat):
    response = messages_pb2.ServerMessage()
    response.fuse_response.status.code = common_messages_pb2.Status.ok
    location_response = prepare_location_response(uuid)

    with reply(endpoint, [response,
                          location_response]) as queue:
        fl.truncate(uuid, 4)
        client_message = queue.get()

    assert client_message.HasField('fuse_request')
    assert client_message.fuse_request.HasField('file_request')

    file_request = client_message.fuse_request.file_request
    assert file_request.HasField('truncate')

    truncate = file_request.truncate
    assert truncate.size == 4
    assert file_request.context_guid == uuid.encode('utf-8')


def test_truncate_should_truncate_open_file(appmock_client, endpoint, fl, uuid):
    fh = do_open(endpoint, fl, uuid, size=10, blocks=[(0, 10)])

    response = messages_pb2.ServerMessage()
    response.fuse_response.status.code = common_messages_pb2.Status.ok

    with reply(endpoint, [response, response]) as queue:
        fl.truncate(uuid, 0)
        client_message = queue.get()

    do_release(endpoint, fl, uuid, fh)


def test_truncate_should_pass_truncate_errors(appmock_client, endpoint, fl, uuid):
    getattr_response = prepare_attr_response(uuid, fuse_messages_pb2.REG)
    response = messages_pb2.ServerMessage()
    response.fuse_response.status.code = common_messages_pb2.Status.eperm

    with pytest.raises(RuntimeError) as excinfo:
        with reply(endpoint, [getattr_response,
                              response]):
            fl.truncate(uuid, 3)

    assert 'Operation not permitted' in str(excinfo.value)


def test_readdir_big_directory(appmock_client, endpoint, fl, uuid):
    chunk_size = 2500
    children_num = 10*chunk_size

    ok = prepare_status_response()
    getattr_response = prepare_attr_response('parentUuid', fuse_messages_pb2.DIR)
    getattr_response2 = prepare_attr_response('parentUuid', fuse_messages_pb2.DIR)

    # Prepare an array of responses of appropriate sizes to client
    # requests
    responses = [getattr_response, ok, ok, ok]
    for i in range(0, int(children_num/chunk_size)):
        repl = fuse_messages_pb2.FileChildrenAttrs()
        for j in range(0, chunk_size):
            link = prepare_attr_response(uuid, fuse_messages_pb2.REG, 1024, 'parentUuid').\
                        fuse_response.file_attr
            link.uuid = ("childUuid_"+str(i)+"_"+str(j)).encode('utf-8')
            link.name = ("file_"+str(i)+"_"+str(j)).encode('utf-8')
            repl.child_attrs.extend([link])

        response = messages_pb2.ServerMessage()
        response.fuse_response.file_children_attrs.CopyFrom(repl)
        response.fuse_response.status.code = common_messages_pb2.Status.ok

        responses.append(response)

    # Prepare empty response after entire directory has been fetched
    # by FsLogic
    empty_repl = fuse_messages_pb2.FileChildrenAttrs()
    empty_repl.child_attrs.extend([])
    empty_repl.is_last = True
    empty_response = messages_pb2.ServerMessage()
    empty_response.fuse_response.file_children_attrs.CopyFrom(empty_repl)
    empty_response.fuse_response.status.code = common_messages_pb2.Status.ok

    responses.append(empty_response)

    assert len(responses) == 4 + children_num/chunk_size + 1

    children = []
    offset = 0

    with reply(endpoint, responses) as queue:
        d = fl.opendir('parentUuid')
        while True:
            children_chunk = fl.readdir('parentUuid', chunk_size, offset)
            client_message = queue.get()
            children.extend(children_chunk)
            if len(children_chunk) < chunk_size:
                break
            offset += len(children_chunk)
        fl.releasedir('parentUuid', d)

    assert len(children) == children_num + 2


def test_write_should_save_blocks(appmock_client, endpoint, fl, uuid):
    fh = do_open(endpoint, fl, uuid, size=5)
    assert 5 == fl.write(uuid, fh, 0, 5)
    assert 5 == len(fl.read(uuid, fh, 0, 10))

    do_release(endpoint, fl, uuid, fh)


def test_read_should_read_partial_content(appmock_client, endpoint, fl, uuid):
    fh = do_open(endpoint, fl, uuid, size=10, blocks=[(4, 6)])
    data = fl.read(uuid, fh, 6, 4)

    assert len(data) == 4

    do_release(endpoint, fl, uuid, fh)


def test_read_should_request_synchronization(appmock_client, endpoint, fl, uuid):
    fh = do_open(endpoint, fl, uuid, size=10, blocks=[(4, 6)])
    sync_response = prepare_sync_response(uuid, '', [(0, 10)])

    appmock_client.reset_tcp_history()
    with reply(endpoint, sync_response) as queue:
        fl.read(uuid, fh, 2, 5)
        client_message = queue.get()

    assert client_message.HasField('fuse_request')
    assert client_message.fuse_request.HasField('file_request')
    file_request = client_message.fuse_request.file_request
    assert file_request.HasField('synchronize_block')
    block = common_messages_pb2.FileBlock()
    block.offset = 2
    block.size = 8
    sync = file_request.synchronize_block
    assert sync.block == block
    assert sync.priority == SYNCHRONIZE_BLOCK_PRIORITY_IMMEDIATE
    assert file_request.context_guid == uuid.encode('utf-8')

    do_release(endpoint, fl, uuid, fh)


def test_read_should_fetch_location_on_invalid_checksum(appmock_client, endpoint, fl, uuid):
    fh = do_open(endpoint, fl, uuid, size=10, blocks=[])
    fl.set_needs_data_consistency_check(True)

    blocks = [(2, 5)]
    responses = []
    # Because the first read is smaller the then minimum default sync range (1MB)
    # client will request a sync from the offset specified (2) to the end of the file
    # as the file is smaller than 1MB
    responses.append(prepare_sync_response(uuid, '', [(2, 10-2)]))
    responses.append(prepare_sync_and_checksum_response(uuid, '', blocks, 'badchecksum'))
    responses.append(prepare_location_response(uuid, blocks))
    responses.append(prepare_location_response(uuid, blocks))

    appmock_client.reset_tcp_history()
    with pytest.raises(RuntimeError) as excinfo:
        with reply(endpoint, responses) as queue:
            fl.read(uuid, fh, 2, 5)
            client_message = queue.get()

    fl.set_needs_data_consistency_check(False)

    assert "Input/output error" in str(excinfo.value)

    do_release(endpoint, fl, uuid, fh)


def test_read_should_retry_request_synchronization(appmock_client, endpoint, fl, uuid):
    fh = do_open(endpoint, fl, uuid, size=10, blocks=[(4, 6)])

    responses = []
    responses.append(prepare_sync_eagain_response(uuid, '', [(2, 8)]))
    responses.append(prepare_sync_response(uuid, '', [(2, 8)]))

    appmock_client.reset_tcp_history()
    with reply(endpoint, responses) as queue:
        fl.read(uuid, fh, 2, 5)
        client_message = queue.get()

    assert client_message.HasField('fuse_request')
    assert client_message.fuse_request.HasField('file_request')
    file_request = client_message.fuse_request.file_request
    assert file_request.HasField('synchronize_block')
    block = common_messages_pb2.FileBlock()
    block.offset = 2
    block.size = 8
    sync = file_request.synchronize_block
    assert sync.block == block
    assert sync.priority == SYNCHRONIZE_BLOCK_PRIORITY_IMMEDIATE
    assert file_request.context_guid == uuid.encode('utf-8')

    do_release(endpoint, fl, uuid, fh)


def test_read_should_retry_canceled_synchronization_request(appmock_client, endpoint, fl, uuid):
    fh = do_open(endpoint, fl, uuid, size=10, blocks=[(4, 6)])

    responses = []
    responses.append(prepare_sync_ecanceled_response(uuid, '', [(2, 8)]))
    responses.append(prepare_sync_ecanceled_response(uuid, '', [(2, 8)]))
    responses.append(prepare_sync_response(uuid, '', [(2, 8)]))

    appmock_client.reset_tcp_history()
    with reply(endpoint, responses) as queue:
        fl.read(uuid, fh, 2, 5)
        client_message = queue.get()

    assert client_message.HasField('fuse_request')
    assert client_message.fuse_request.HasField('file_request')
    file_request = client_message.fuse_request.file_request
    assert file_request.HasField('synchronize_block')
    block = common_messages_pb2.FileBlock()
    block.offset = 2
    block.size = 8
    sync = file_request.synchronize_block
    assert sync.block == block
    assert sync.priority == SYNCHRONIZE_BLOCK_PRIORITY_IMMEDIATE
    assert file_request.context_guid == uuid.encode('utf-8')

    do_release(endpoint, fl, uuid, fh)


def test_read_should_not_retry_request_synchronization_too_many_times(appmock_client, endpoint, fl, uuid):
    fh = do_open(endpoint, fl, uuid, size=10, blocks=[(4, 6)])

    responses = []
    responses.append(prepare_sync_eagain_response(uuid, '', [(2, 8)]))
    responses.append(prepare_sync_eagain_response(uuid, '', [(2, 8)]))
    responses.append(prepare_sync_eagain_response(uuid, '', [(2, 8)]))
    responses.append(prepare_sync_eagain_response(uuid, '', [(2, 8)]))

    appmock_client.reset_tcp_history()
    with pytest.raises(RuntimeError) as excinfo:
        with reply(endpoint, responses) as queue:
            fl.read(uuid, fh, 2, 5)
            client_message = queue.get()

    assert 'Resource temporarily unavailable' in str(excinfo.value)

    do_release(endpoint, fl, uuid, fh)


def test_read_should_continue_reading_after_synchronization(appmock_client,
                                                            endpoint, fl, uuid):
    fh = do_open(endpoint, fl, uuid, size=10, blocks=[(4, 6)])
    sync_response = prepare_sync_response(uuid, '', [(0, 10)])

    appmock_client.reset_tcp_history()
    with reply(endpoint, sync_response):
        assert 5 == len(fl.read(uuid, fh, 2, 5))

    do_release(endpoint, fl, uuid, fh)


def test_read_should_continue_reading_after_synchronization_partial(appmock_client,
                                                            endpoint, fl, uuid):
    fh = do_open(endpoint, fl, uuid, size=10, blocks=[(4, 6)])
    sync_response = prepare_partial_sync_response(uuid, '', [(0, 10)], 0, 10)

    appmock_client.reset_tcp_history()
    with reply(endpoint, sync_response):
        assert 5 == len(fl.read(uuid, fh, 2, 5))

    do_release(endpoint, fl, uuid, fh)


def test_read_should_should_open_file_block_once(appmock_client, endpoint, fl, uuid):
    fh = do_open(endpoint, fl, uuid, size=10, blocks=[
        (0, 5, 'storage1', 'file1'), (5, 5, 'storage2', 'file2')])

    fl.expect_call_sh_open("file1", 1)
    fl.expect_call_sh_open("file2", 1)

    assert 5 == len(fl.read(uuid, fh, 0, 5))
    assert 5 == len(fl.read(uuid, fh, 5, 5))

    assert 5 == len(fl.read(uuid, fh, 0, 5))
    assert 5 == len(fl.read(uuid, fh, 0, 5))

    assert 5 == len(fl.read(uuid, fh, 5, 5))
    assert 5 == len(fl.read(uuid, fh, 5, 5))

    assert fl.verify_and_clear_expectations()

    do_release(endpoint, fl, uuid, fh)


def test_release_should_release_open_file_blocks(appmock_client, endpoint, fl, uuid):
    fh = do_open(endpoint, fl, uuid, size=10, blocks=[
        (0, 5, 'storage1', 'file1'), (5, 5, 'storage2', 'file2')])

    assert 5 == len(fl.read(uuid, fh, 0, 5))
    assert 5 == len(fl.read(uuid, fh, 5, 5))

    fl.expect_call_sh_release('file1', 1)
    fl.expect_call_sh_release('file2', 1)

    do_release(endpoint, fl, uuid, fh)

    assert fl.verify_and_clear_expectations()

@pytest.mark.skip()
def test_release_should_pass_helper_errors(appmock_client, endpoint, fl, uuid):
    fh = do_open(endpoint, fl, uuid, size=5, blocks=[
        (0, 5, 'storage1', 'file1')])

    with pytest.raises(RuntimeError) as excinfo:
        fl.failHelper()
        fl.read(uuid, fh, 0, 5)

    fl.expect_call_sh_release('file1', 1)

    do_release(endpoint, fl, uuid, fh)

    assert 'Owner died' in str(excinfo.value)


def test_release_should_send_release_message(appmock_client, endpoint, fl, uuid):
    fh = do_open(endpoint, fl, uuid, size=0)

    sent_messages = do_release(endpoint, fl, uuid, fh)

    sent_messages.get() # skip fsync message
    client_message = sent_messages.get()
    assert client_message.HasField('fuse_request')
    assert client_message.fuse_request.HasField('file_request')
    assert client_message.fuse_request.file_request.HasField('release')


def test_release_should_send_fsync_message(appmock_client, endpoint, fl, uuid):
    fh = do_open(endpoint, fl, uuid, size=0)

    sent_messages = do_release(endpoint, fl, uuid, fh)

    client_message = sent_messages.get()

    assert client_message.HasField('fuse_request')
    assert client_message.fuse_request.HasField('file_request')
    assert client_message.fuse_request.file_request.HasField('fsync')


@pytest.mark.skip
def test_fslogic_should_handle_processing_status_message(appmock_client, endpoint, fl, uuid):
    ok = prepare_status_response()
    getattr_response = \
        prepare_attr_response(uuid, fuse_messages_pb2.DIR, 0, 'parentUuid', 'name')
    getattr_newuuid_response = \
        prepare_attr_response('newUuid', fuse_messages_pb2.DIR, 0, 'parentUuid', 'name')
    getattr_parent_response = \
        prepare_attr_response('parentUuid', fuse_messages_pb2.DIR)
    getattr_newparent_response = \
        prepare_attr_response('newParentUuid', fuse_messages_pb2.DIR)
    rename_response = prepare_rename_response('newUuid')
    processing_status_responses = \
        [prepare_processing_status_response(messages_pb2.IN_PROGRESS)
                for _ in range(5)]

    responses = [ok, ok, ok, getattr_response, getattr_parent_response]
    responses.extend(processing_status_responses)
    responses.append(rename_response)
    responses.append(getattr_newuuid_response)

    with reply(endpoint, responses) as queue:
        fl.rename('parentUuid', 'name', 'newParentUuid', 'newName')
        queue.get()
        queue.get()
        queue.get()
        queue.get()
        queue.get()
        client_message = queue.get()

    assert client_message.HasField('fuse_request')
    assert client_message.fuse_request.HasField('file_request')

    file_request = client_message.fuse_request.file_request
    assert file_request.HasField('rename')

    rename = file_request.rename
    assert rename.target_parent_uuid == b'newParentUuid'
    assert rename.target_name == b'newName'
    assert file_request.context_guid == \
           getattr_response.fuse_response.file_attr.uuid


def prepare_listxattr_response(uuid):
    repl = fuse_messages_pb2.XattrList()

    repl.names.extend([b"xattr1", b"xattr2", b"xattr3", b"xattr4"])

    server_response = messages_pb2.ServerMessage()
    server_response.fuse_response.xattr_list.CopyFrom(repl)
    server_response.fuse_response.status.code = common_messages_pb2.Status.ok

    return server_response


def test_listxattrs_should_return_listxattrs(endpoint, fl, uuid):
    getattr_response = prepare_attr_response(uuid, fuse_messages_pb2.REG)
    listxattr_response = prepare_listxattr_response(uuid)

    listxattrs = []
    with reply(endpoint, [listxattr_response,
                          getattr_response]) as queue:
        listxattrs = fl.listxattr(uuid)
        client_message = queue.get()

    assert client_message.HasField('fuse_request')
    assert client_message.fuse_request.HasField('file_request')

    file_request = client_message.fuse_request.file_request
    assert file_request.HasField('list_xattr')
    assert file_request.context_guid == uuid.encode('utf-8')

    assert listxattr_response.status.code == common_messages_pb2.Status.ok
    assert "xattr1" in set(listxattrs)
    assert "xattr2" in set(listxattrs)
    assert "xattr3" in set(listxattrs)
    assert "xattr4" in set(listxattrs)


def prepare_getxattr_response(uuid, name, value):
    repl = fuse_messages_pb2.Xattr()

    repl.name = name.encode('utf-8')
    repl.value = value.encode('utf-8')

    server_response = messages_pb2.ServerMessage()
    server_response.fuse_response.xattr.CopyFrom(repl)
    server_response.fuse_response.status.code = common_messages_pb2.Status.ok

    return server_response


def test_getxattr_should_return_xattr(endpoint, fl, uuid):
    xattr_name = "org.onedata.acl"
    xattr_value = "READ | WRITE | DELETE"
    response = prepare_getxattr_response(uuid, xattr_name, xattr_value)

    xattr = None
    with reply(endpoint, response) as queue:
        xattr = fl.getxattr(uuid, xattr_name)
        client_message = queue.get()

    assert client_message.HasField('fuse_request')
    assert client_message.fuse_request.HasField('file_request')

    file_request = client_message.fuse_request.file_request
    assert file_request.HasField('get_xattr')

    assert xattr.name == xattr_name
    assert xattr.value == xattr_value


def test_getxattr_should_return_enoattr_for_invalid_xattr(endpoint, fl, uuid):
    response = messages_pb2.ServerMessage()
    response.fuse_response.status.code = common_messages_pb2.Status.enodata

    with pytest.raises(RuntimeError) as excinfo:
        with reply(endpoint, response):
            fl.getxattr(uuid, "org.onedata.dontexist")

    assert 'No data available' in str(excinfo.value)


def test_setxattr_should_set_xattr(endpoint, fl, uuid):
    xattr_name = "org.onedata.acl"
    xattr_value = "READ | WRITE | DELETE"
    response = messages_pb2.ServerMessage()
    response.fuse_response.status.code = common_messages_pb2.Status.ok

    with reply(endpoint, response) as queue:
        fl.setxattr(uuid, xattr_name, xattr_value, False, False)
        client_message = queue.get()

    assert client_message.HasField('fuse_request')
    assert client_message.fuse_request.HasField('file_request')

    file_request = client_message.fuse_request.file_request
    assert file_request.HasField('set_xattr')
    assert file_request.set_xattr.HasField('xattr')

    assert file_request.set_xattr.xattr.name == xattr_name.encode('utf-8')
    assert file_request.set_xattr.xattr.value == xattr_value.encode('utf-8')


def test_setxattr_should_set_xattr_with_binary_data(endpoint, fl, uuid):
    xattr_name = "org.onedata.acl"
    xattr_value = b'BEGINSTRINGWITHNULLS\x00\x0F\x00\x0F\x00\x0F\x00\x0F\x00\x0F\x00\x0FENDSTRINGWITHNULLS'
    response = messages_pb2.ServerMessage()
    response.fuse_response.status.code = common_messages_pb2.Status.ok

    with reply(endpoint, response) as queue:
        fl.setxattr(uuid, xattr_name, xattr_value, False, False)
        client_message = queue.get()

    assert client_message.HasField('fuse_request')
    assert client_message.fuse_request.HasField('file_request')
    file_request = client_message.fuse_request.file_request

    assert file_request.HasField('set_xattr')
    assert file_request.set_xattr.HasField('xattr')

    assert file_request.set_xattr.xattr.name == xattr_name.encode('utf-8')
    assert file_request.set_xattr.xattr.value == xattr_value


def test_setxattr_should_set_xattr_with_long_value(endpoint, fl, uuid):
    xattr_name = "org.onedata.acl"
    xattr_value = "askljdhflajkshdfjklhasjkldfhajklshdfljkashdfjklhasljkdhfjklashdfjklhasljdfhljkashdfljkhasjkldfhkljasdfhaslkdhfljkashdfljkhasdjklfhajklsdhfljkashdflkjhasjkldfhlakjsdhflkjahsfjklhasdjklfghlajksdgjklashfjklashfljkahsdljkfhasjkldfhlkajshdflkjahsdfljkhasldjkfhlkashdflkjashdfljkhasldkjfhalksdhfljkashdfljkhasdlfjkhaljksdhfjklashdfjklhasjkldfhljkasdhfljkashdlfjkhasldjkfhaljskdhfljkashdfljkhaspeuwshfiuawhgelrfihjasdgffhjgsdfhjgaskhjdfgjkaszgdfjhasdkfgaksjdfgkjahsdgfkhjasgdfkjhagsdkhjfgakhsjdfgkjhasgdfkhjgasdkjhfgakjshdgfkjhasgdkjhfgaskjhdfgakjhsdgfkjhasdgfkjhagsdkfhjgaskjdhfgkajsgdfkhjagsdkfjhgasdkjhfgaksjhdgfkajshdgfkjhasdgfkjhagskjdhfgakjshdgfkhjasdgfkjhasgdkfhjgaskdhjfgaksjdfgkasjdhgfkajshdgfkjhasgdfkhjagskdhjfgaskhjdfgkjasdhgfkjasgdkhjasdgkfhjgaksjhdfgkajshdgfkjhasdgfkjhagsdhjkfgaskhjdfgahjksdgfkhjasdgfhasgdfjhgaskdhjfgadkshjgfakhjsdgfkjhadsgkfhjagshjkdfgadhjsaskljdhflajkshdfjklhasjkldfhajklshdfljkashdfjklhasljkdhfjklashdfjklhasljdfhljkashdfljkhasjkldfhkljasdfhaslkdhfljkashdfljkhasdjklfhajklsdhfljkashdflkjhasjkldfhlakjsdhflkjahsfjklhasdjklfghlajksdgjklashfjklashfljkahsdljkfhasjkldfhlkajshdflkjahsdfljkhasldjkfhlkashdflkjashdfljkhasldkjfhalksdhfljkashdfljkhasdlfjkhaljksdhfjklashdfjklhasjkldfhljkasdhfljkashdlfjkhasldjkfhaljskdhfljkashdfljkhaspeuwshfiuawhgelrfihjasdgffhjgsdfhjgaskhjdfgjkaszgdfjhasdkfgaksjdfgkjahsdgfkhjasgdfkjhagsdkhjfgakhsjdfgkjhasgdfkhjgasdkjhfgakjshdgfkjhasgdkjhfgaskjhdfgakjhsdgfkjhasdgfkjhagsdkfhjgaskjdhfgkajsgdfkhjagsdkfjhgasdkjhfgaksjhdgfkajshdgfkjhasdgfkjhagskjdhfgakjshdgfkhjasdgfkjhasgdkfhjgaskdhjfgaksjdfgkasjdhgfkajshdgfkjhasgdfkhjagskdhjfgaskhjdfgkjasdhgfkjasgdkhjasdgkfhjgaksjhdfgkajshdgfkjhasdgfkjhagsdhjkfgaskhjdfgahjksdgfkhjasdgfhasgdfjhgaskdhjfgadkshjgfakhjsdgfkjhadsgkfhjagshjkdfgadhjsaskljdhflajkshdfjklhasjkldfhajklshdfljkashdfjklhasljkdhfjklashdfjklhasljdfhljkashdfljkhasjkldfhkljasdfhaslkdhfljkashdfljkhasdjklfhajklsdhfljkashdflkjhasjkldfhlakjsdhflkjahsfjklhasdjklfghlajksdgjklashfjklashfljkahsdljkfhasjkldfhlkajshdflkjahsdfljkhasldjkfhlkashdflkjashdfljkhasldkjfhalksdhfljkashdfljkhasdlfjkhaljksdhfjklashdfjklhasjkldfhljkasdhfljkashdlfjkhasldjkfhaljskdhfljkashdfljkhaspeuwshfiuawhgelrfihjasdgffhjgsdfhjgaskhjdfgjkaszgdfjhasdkfgaksjdfgkjahsdgfkhjasgdfkjhagsdkhjfgakhsjdfgkjhasgdfkhjgasdkjhfgakjshdgfkjhasgdkjhfgaskjhdfgakjhsdgfkjhasdgfkjhagsdkfhjgaskjdhfgkajsgdfkhjagsdkfjhgasdkjhfgaksjhdgfkajshdgfkjhasdgfkjhagskjdhfgakjshdgfkhjasdgfkjhasgdkfhjgaskdhjfgaksjdfgkasjdhgfkajshdgfkjhasgdfkhjagskdhjfgaskhjdfgkjasdhgfkjasgdkhjasdgkfhjgaksjhdfgkajshdgfkjhasdgfkjhagsdhjkfgaskhjdfgahjksdgfkhjasdgfhasgdfjhgaskdhjfgadkshjgfakhjsdgfkjhadsgkfhjagshjkdfgadhjsaskljdhflajkshdfjklhasjkldfhajklshdfljkashdfjklhasljkdhfjklashdfjklhasljdfhljkashdfljkhasjkldfhkljasdfhaslkdhfljkashdfljkhasdjklfhajklsdhfljkashdflkjhasjkldfhlakjsdhflkjahsfjklhasdjklfghlajksdgjklashfjklashfljkahsdljkfhasjkldfhlkajshdflkjahsdfljkhasldjkfhlkashdflkjashdfljkhasldkjfhalksdhfljkashdfljkhasdlfjkhaljksdhfjklashdfjklhasjkldfhljkasdhfljkashdlfjkhasldjkfhaljskdhfljkashdfljkhaspeuwshfiuawhgelrfihjasdgffhjgsdfhjgaskhjdfgjkaszgdfjhasdkfgaksjdfgkjahsdgfkhjasgdfkjhagsdkhjfgakhsjdfgkjhasgdfkhjgasdkjhfgakjshdgfkjhasgdkjhfgaskjhdfgakjhsdgfkjhasdgfkjhagsdkfhjgaskjdhfgkajsgdfkhjagsdkfjhgasdkjhfgaksjhdgfkajshdgfkjhasdgfkjhagskjdhfgakjshdgfkhjasdgfkjhasgdkfhjgaskdhjfgaksjdfgkasjdhgfkajshdgfkjhasgdfkhjagskdhjfgaskhjdfgkjasdhgfkjasgdkhjasdgkfhjgaksjhdfgkajshdgfkjhasdgfkjhagsdhjkfgaskhjdfgahjksdgfkhjasdgfhasgdfjhgaskdhjfgadkshjgfakhjsdgfkjhadsgkfhjagshjkdfgadhjs"

    response = messages_pb2.ServerMessage()
    response.fuse_response.status.code = common_messages_pb2.Status.ok

    with reply(endpoint, response) as queue:
        fl.setxattr(uuid, xattr_name, xattr_value, False, False)
        client_message = queue.get()

    assert client_message.HasField('fuse_request')
    assert client_message.fuse_request.HasField('file_request')
    file_request = client_message.fuse_request.file_request

    assert file_request.HasField('set_xattr')
    assert file_request.set_xattr.HasField('xattr')

    assert file_request.set_xattr.xattr.name == xattr_name.encode('utf-8')
    assert file_request.set_xattr.xattr.value == xattr_value.encode('utf-8')


def test_removexattr_should_remove_xattr(endpoint, fl, uuid):
    xattr_name = "org.onedata.acl"
    response = messages_pb2.ServerMessage()
    response.fuse_response.status.code = common_messages_pb2.Status.ok

    with reply(endpoint, response) as queue:
        fl.removexattr(uuid, xattr_name)
        client_message = queue.get()

    assert client_message.HasField('fuse_request')
    assert client_message.fuse_request.HasField('file_request')
    file_request = client_message.fuse_request.file_request
    assert file_request.context_guid == uuid.encode('utf-8')

    remove_xattr_request = file_request.remove_xattr
    assert remove_xattr_request.HasField('name')
    assert remove_xattr_request.name == xattr_name.encode('utf-8')


def test_removexattr_should_return_enoattr_for_invalid_xattr(endpoint, fl, uuid):
    response = messages_pb2.ServerMessage()
    response.fuse_response.status.code = common_messages_pb2.Status.enodata

    with pytest.raises(RuntimeError) as excinfo:
        with reply(endpoint, response):
            fl.removexattr(uuid, "org.onedata.dontexist")

    assert 'No data available' in str(excinfo.value)


@pytest.mark.skip
def test_readdir_should_handle_archivematica_metadata(appmock_client, endpoint, fl_archivematica):
    parentUuid = 'parentParentUuid'
    uuid = 'parentUuid'

    getattr_reponse = prepare_attr_response(uuid, fuse_messages_pb2.DIR, parent_uuid=parentUuid)
    getattr_reponse_parent = prepare_attr_response(parentUuid, fuse_messages_pb2.DIR)

    #
    # Prepare first response with 5 files
    #
    repl1 = prepare_file_children_attr_response(uuid, "afiles-", 5)
    repl1.is_last = False

    response1 = messages_pb2.ServerMessage()
    response1.fuse_response.file_children_attrs.CopyFrom(repl1)
    response1.fuse_response.status.code = common_messages_pb2.Status.ok

    #
    # Prepare second response with another 5 file
    #
    repl2 = prepare_file_children_attr_response(uuid, "bfiles-", 5)
    repl2.is_last = True

    response2 = messages_pb2.ServerMessage()
    response2.fuse_response.file_children_attrs.CopyFrom(repl2)
    response2.fuse_response.status.code = common_messages_pb2.Status.ok

    children = []
    offset = 0
    chunk_size = 50
    uuid_am = uuid+".__onedata_archivematica"
    with reply(endpoint, [getattr_reponse,
                          getattr_reponse_parent,
                          response1,
                          response2]) as queue:
        parentAttr = fl_archivematica.lookup(parentUuid, "test.__onedata_archivematica")
        d = fl_archivematica.opendir(uuid_am)
        children_chunk = fl_archivematica.readdir(uuid_am, chunk_size, offset)
        _ = queue.get()
        fl_archivematica.releasedir(uuid_am, d)
        assert len(children_chunk) == 14
        assert "processingMCP.xml" in children_chunk
        assert "metadata" in children_chunk


@pytest.mark.skip
def test_read_should_read_archivematica_processingmcp(appmock_client, endpoint, fl_archivematica):
    parentUuid = 'parentParentUuid'
    uuid = 'parentUuid'

    getattr_reponse = prepare_attr_response(
            uuid, fuse_messages_pb2.DIR, parent_uuid=parentUuid, name="test")
    getattr_reponse_parent = prepare_attr_response(parentUuid, fuse_messages_pb2.DIR)

    #
    # Prepare first response with 5 files
    #
    repl1 = prepare_file_children_attr_response(uuid, "afiles-", 5)
    repl1.is_last = False

    response1 = messages_pb2.ServerMessage()
    response1.fuse_response.file_children_attrs.CopyFrom(repl1)
    response1.fuse_response.status.code = common_messages_pb2.Status.ok

    #
    # Prepare second response with another 5 file
    #
    repl2 = prepare_file_children_attr_response(uuid, "bfiles-", 5)
    repl2.is_last = True

    response2 = messages_pb2.ServerMessage()
    response2.fuse_response.file_children_attrs.CopyFrom(repl2)
    response2.fuse_response.status.code = common_messages_pb2.Status.ok

    #
    # Prepare __archivematica metadata response
    #
    xattr_name = "onedata_json"
    xattr_value = """
{
  "__onedata": {
    "__archivematica": {
      "processingMCP": {
          "preconfiguredChoices": {
          "preconfiguredChoice": [
              {
                "appliesTo": "1ba589db-88d1-48cf-bb1a-a5f9d2b17378",
                "goToChain": "0a24787c-00e3-4710-b324-90e792bfb484"
              },
              {
                "appliesTo": "1c2550f1-3fc0-45d8-8bc4-4c06d720283b",
                "goToChain": "0a24787c-00e3-4710-b324-90e792bfb484"
              },
              {
                "appliesTo": "5e58066d-e113-4383-b20b-f301ed4d751c",
                "goToChain": "4500f34e-f004-4ccf-8720-5c38d0be2254"
              },
              {
                "appliesTo": "01c651cb-c174-4ba4-b985-1d87a44d6754",
                "goToChain": "ecfad581-b007-4612-a0e0-fcc551f4057f"
              }
          ]
        }
      }
    }
  }
}
"""
    am_metadata_response = prepare_getxattr_response(uuid, xattr_name, xattr_value)

    children = []
    offset = 0
    chunk_size = 50
    with reply(endpoint, [getattr_reponse,
                          getattr_reponse_parent,
                          response1,
                          response2,
                          am_metadata_response]) as queue:
        parentAttr = fl_archivematica.lookup(parentUuid, "test.__onedata_archivematica")

        # First try to open the 'test' directory as is
        d = fl_archivematica.opendir(uuid)
        children_chunk = fl_archivematica.readdir(uuid, chunk_size, offset)
        _ = queue.get()
        fl_archivematica.releasedir(uuid, d)
        assert len(children_chunk) == 12
        assert "processingMCP.xml" not in children_chunk

        uuid_am = uuid + ".__onedata_archivematica"

        # Now try to open the 'test' directory in Archivematica mode
        d = fl_archivematica.opendir(uuid_am)
        children_chunk = fl_archivematica.readdir(uuid_am, chunk_size, offset)
        _ = queue.get()
        fl_archivematica.releasedir(uuid_am, d)
        assert len(children_chunk) == 14
        assert "processingMCP.xml" in children_chunk

        assert fl_archivematica.metadata_cache_contains(uuid+"-processing-mcp")
        assert fl_archivematica.metadata_cache_contains(uuid+"-metadata")

        attr = fl_archivematica.lookup(uuid_am, "processingMCP.xml")
        fh = fl_archivematica.open(uuid+"-processing-mcp", 0)
        data = fl_archivematica.read(uuid+"-processing-mcp", fh, 0, 4096)
        fl_archivematica.release(uuid+"-processing-mcp", fh)

        processing_mcp = ET.fromstring(data)
        assert processing_mcp.tag == "processingMCP"
        preconfigured_choices = processing_mcp.find("preconfiguredChoices")
        assert preconfigured_choices.tag == "preconfiguredChoices"
        choice_list = preconfigured_choices.findall("preconfiguredChoice")
        assert len(choice_list) == 4


@pytest.mark.skip
def test_read_should_read_archivematica_metadata_json(appmock_client, endpoint, fl_archivematica):
    parentUuid = 'parentParentUuid'
    uuid = 'parentUuid'

    getattr_reponse = prepare_attr_response(uuid, fuse_messages_pb2.DIR, parent_uuid=parentUuid)
    getattr_reponse_parent = prepare_attr_response(parentUuid, fuse_messages_pb2.DIR)

    repl1 = prepare_file_children_attr_response(uuid, "afiles-", 1)
    repl1.is_last = True

    dir1_response = prepare_attr_response(
            "dir1Uuid", fuse_messages_pb2.DIR, parent_uuid=uuid, name='dir1')
    repl1.child_attrs.extend([dir1_response.fuse_response.file_attr])

    response1 = messages_pb2.ServerMessage()
    response1.fuse_response.file_children_attrs.CopyFrom(repl1)
    response1.fuse_response.status.code = common_messages_pb2.Status.ok

    xattr_list1 = fuse_messages_pb2.XattrList()
    xattr_list1.names.extend(["onedata_json", "dc.license"])
    xattr_list1_response = messages_pb2.ServerMessage()
    xattr_list1_response.fuse_response.xattr_list.CopyFrom(xattr_list1)
    xattr_list1_response.fuse_response.status.code = common_messages_pb2.Status.ok

    xattr_list2 = fuse_messages_pb2.XattrList()
    xattr_list2.names.extend(["onedata_json"])
    xattr_list2_response = messages_pb2.ServerMessage()
    xattr_list2_response.fuse_response.xattr_list.CopyFrom(xattr_list2)
    xattr_list2_response.fuse_response.status.code = common_messages_pb2.Status.ok

    xattr_value1 = """{"dc.language":"CSV","dc.identifier":"123"}"""
    metadata_response1 = prepare_getxattr_response(uuid, "onedata_json", xattr_value1)
    xattr_value2 = "CC-0"
    metadata_response2 = prepare_getxattr_response(uuid, "dc.license", xattr_value2)

    xattr_value3 = """{"dc.language":"CSV","dc.identifier":"456"}"""
    metadata_response3 = prepare_getxattr_response(uuid, "onedata_json", xattr_value3)

    repl2 = prepare_file_children_attr_response("dir1Uuid", "bfiles-", 1)
    repl2.is_last = True

    response2 = messages_pb2.ServerMessage()
    response2.fuse_response.file_children_attrs.CopyFrom(repl2)
    response2.fuse_response.status.code = common_messages_pb2.Status.ok

    children = []
    offset = 0
    chunk_size = 50
    uuid_am = uuid+".__onedata_archivematica"
    with reply(endpoint, [getattr_reponse,
                          getattr_reponse_parent,
                          response1,
                          response2,
                          xattr_list1_response,
                          metadata_response1,
                          metadata_response2,
                          xattr_list2_response,
                          metadata_response3]) as queue:
        parentAttr = fl_archivematica.lookup(parentUuid, "test.__onedata_archivematica")
        d = fl_archivematica.opendir(uuid_am)
        children_chunk = fl_archivematica.readdir(uuid_am, chunk_size, offset)
        _ = queue.get()
        fl_archivematica.releasedir(uuid_am, d)
        assert len(children_chunk) == 6
        assert "metadata" in children_chunk

        assert fl_archivematica.metadata_cache_contains(uuid+"-metadata")

        metadataUuid = uuid+"-metadata"
        d = fl_archivematica.opendir(metadataUuid)
        children_chunk = fl_archivematica.readdir(metadataUuid, chunk_size, 0)
        fl_archivematica.release(uuid, d)

        assert len(children_chunk) == 3
        assert "metadata.json" in list(children_chunk)

        attr = fl_archivematica.lookup(metadataUuid, "metadata.json")
        fh = fl_archivematica.open(metadataUuid+"-metadata-json", 0)
        data = fl_archivematica.read(metadataUuid+"-metadata-json", fh, 0, 1024)
        fl_archivematica.release(metadataUuid+"-metadata-json", fh)

        am_metadata = json.loads(data)
        assert len(am_metadata) == 2
        assert am_metadata[0]["filename"] == "objects/dir1/bfiles-0"
        assert am_metadata[1]["filename"] == "objects/afiles-0"
