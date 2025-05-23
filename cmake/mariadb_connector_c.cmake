SET(OPT CONC_)

IF (CMAKE_BUILD_TYPE STREQUAL "Debug")
  SET(CONC_WITH_RTC ON)
ENDIF()

SET(CONC_WITH_SIGNCODE ${SIGNCODE})
SET(SIGN_OPTIONS ${SIGNTOOL_PARAMETERS})
SET(CONC_WITH_EXTERNAL_ZLIB ON)
SET(CLIENT_PLUGIN_PARSEC OFF)

CHECK_INCLUDE_FILES (threads.h HAVE_THREADS_H)
IF(HAVE_THREADS_H)
SET(CLIENT_PLUGIN_PARSEC DYNAMIC)
ENDIF()

IF(NOT CONC_WITH_SSL)
  IF(SSL_DEFINES MATCHES "WOLFSSL")
    IF(WIN32)
      SET(CONC_WITH_SSL "SCHANNEL")
    ELSE()
      SET(CONC_WITH_SSL "GNUTLS") # that's what debian wants, right?
    ENDIF()
  ELSE()
    SET(CONC_WITH_SSL "OPENSSL")
    SET(OPENSSL_FOUND TRUE)
  ENDIF()
ENDIF()

SET(CONC_WITH_CURL OFF)
SET(CONC_WITH_MYSQLCOMPAT ON)

IF (INSTALL_LAYOUT STREQUAL "RPM")
  SET(CONC_INSTALL_LAYOUT "RPM")
ELSEIF (INSTALL_LAYOUT STREQUAL "DEB")
  SET(CONC_INSTALL_LAYOUT "DEB")
ELSE()
  SET(CONC_INSTALL_LAYOUT "DEFAULT")
ENDIF()

IF(WITH_BOOST_CONTEXT)
  SET(CONC_WITH_BOOST_CONTEXT ON)
ENDIF()

SET(PLUGIN_INSTALL_DIR ${INSTALL_PLUGINDIR})
SET(MARIADB_UNIX_ADDR ${MYSQL_UNIX_ADDR})

SET(CLIENT_PLUGIN_PVIO_NPIPE STATIC)
SET(CLIENT_PLUGIN_PVIO_SHMEM STATIC)
SET(CLIENT_PLUGIN_PVIO_SOCKET STATIC)

MESSAGE("== Configuring MariaDB Connector/C")
ADD_SUBDIRECTORY(libmariadb)

IF(MSVC AND TARGET mariadb_obj AND TARGET mariadbclient)
  # With MSVC, do not produce LTCG-compiled static client libraries.
  # They are not usable by end-users, being tied to exact compiler version
  MAYBE_DISABLE_IPO(mariadb_obj)
  MAYBE_DISABLE_IPO(mariadbclient)
ENDIF()

IF(UNIX)
  INSTALL(CODE "EXECUTE_PROCESS(
                  COMMAND ${CMAKE_COMMAND} -E make_directory \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${INSTALL_BINDIR})
                EXECUTE_PROCESS(
                  COMMAND ${CMAKE_COMMAND} -E create_symlink mariadb_config ${INSTALL_BINDIR}/mariadb-config
                  WORKING_DIRECTORY \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX})"
          COMPONENT Development)
ENDIF()

GET_DIRECTORY_PROPERTY(MARIADB_CONNECTOR_C_VERSION DIRECTORY libmariadb DEFINITION CPACK_PACKAGE_VERSION)
MESSAGE1(MARIADB_CONNECTOR_C_VERSION "MariaDB Connector/C ${MARIADB_CONNECTOR_C_VERSION}")
