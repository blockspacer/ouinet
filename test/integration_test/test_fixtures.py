# Copyright 2018 eQualit.ie
# See LICENSE for other credits and copying information

# Constants used in the test
import logging

class TestFixtures:
    LOGGING_LEVEL = logging.DEBUG #Change true to turn on debugging 
    KEEP_IO_ALIVE_PULSE_INTERVAL = 60 #seconds
    
    FATAL_ERROR_INDICATOR_REGEX = r'[\s\S]*\[ABORT\][\s\S]*'
    DEFAULT_PROCESS_TIMEOUT = 15 # seconds
    TCP_TRANSPORT_TIMEOUT = 15
    I2P_TRANSPORT_TIMEOUT = 600
    IPFS_CACHE_TIMEOUT = 900
    
    TEST_TIMEOUT = {
        "test_i2p_transport":600,
        "test_tcp_transport":15,
        "test_ipfs_cache":900}

    #BENCHMARK REGEX INDICES
    READY_REGEX_INDEX = 0
    REQUEST_CACHED_REGEX_INDEX = 1
    
    
    REPO_FOLDER_NAME = "repos"

    INJECTOR_CONF_FILE_NAME = "ouinet-injector.conf"
    INJECTOR_CONF_FILE_CONTENT = "open-file-limit = 32768\n"

    I2P_INJECTOR_NAME = "i2p_injector"
    I2P_TUNNEL_READY_REGEX = r'[\s\S]*I2P Tunnel has been established'

    I2P_CLIENT = {"name":"i2p_client", "port": 8081}

    MAX_NO_OF_I2P_CLIENTS = 5
    MAX_NO_OF_TRIAL_I2P_REQUESTS = 5
    
    TCP_INJECTOR_NAME = "tcp_injector"
    TCP_PORT_READY_REGEX = r'[\s\S]*Successfully listening on TCP Port[\s\S]*'
    TCP_INJECTOR_PORT = 7070

    CACHE_INJECTOR_NAME = "cache_injector"
    
    TEST_PAGE_BODY="<html><body>TESTPAGE</body></html>\n"
    TEST_HTTP_SERVER_PORT = 7080
    RESPONSE_LENGTH = 20

    CLIENT_CONFIG_FILE_NAME = "ouinet-client.conf"

    TCP_CLIENT = { "name": "tcp_client",
                         "port": 8081}

    CACHE_CLIENT = [{ "name": "cache_client_1",
                         "port": 8084},
                      { "name": "cache_client_2",
                         "port": 8085}]

    FIRST_CLIENT_CONF_FILE_CONTENT = "open-file-limit = 4096\n"

    IPNS_ID_ANNOUNCE_REGEX = "[\s\S]*IPNS DB: ([A-Za-z0-9]+)[\s\S]*"
    START_OF_IPNS_RESOLUTION_REGEX = r'[\s\S]*Resolving IPNS address: [\s\S]*'
    IPFS_CACHE_READY_REGEX = r'[\s\S]*IPNS ID has been resolved successfully[\s\S]*'
    REQUEST_CACHED_REGEX = r'[\s\S]*Request was successfully published to cache[\s\S]*'
    NO_OF_CACHED_MESSAGES_REQUIRED = 1
    RETRIEVED_FROM_CACHE_REGEX = r'[\s\S]*Response was retrieved from cache[\s\S]*'
    MAX_NO_OF_TRIAL_IPFS_CACHE_REQUESTS = 3

    I2P_DHT_ADVERTIZE_WAIT_PERIOD = 30
    I2P_TUNNEL_HEALING_PERIOD = 10
