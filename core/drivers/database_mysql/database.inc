<?php

/**
 * @file
 * Database interface code for MySQL database servers.
 */

/**
 * @addtogroup database
 * @{
 */

class DatabaseConnection_mysql extends DatabaseConnection {

  /**
   * Flag to indicate if the cleanup function in __destruct() should run.
   *
   * @var boolean
   */
  protected $needsCleanup = FALSE;

  /**
   * Flag set in utf8mb4IsSupported().
   *
   * @var boolean
   */
  protected $utf8mb4Supported;

  public function __construct(array $connection_options = array()) {
    // This driver defaults to transaction support, except if explicitly passed FALSE.
    $this->transactionSupport = !isset($connection_options['transactions']) || ($connection_options['transactions'] !== FALSE);

    // MySQL never supports transactional DDL.
    $this->transactionalDDLSupport = FALSE;

    $this->connectionOptions = $connection_options;

    $charset = 'utf8';
    // Check if the charset is overridden to utf8mb4 in settings.php.
    if ($this->utf8mb4IsActive()) {
      $charset = 'utf8mb4';
    }

    // The DSN should use either a socket or a host/port.
    if (isset($connection_options['unix_socket'])) {
      $dsn = 'mysql:unix_socket=' . $connection_options['unix_socket'];
    }
    else {
      // Default to TCP connection on port 3306.
      $dsn = 'mysql:host=' . $connection_options['host'] . ';port=' . (empty($connection_options['port']) ? 3306 : $connection_options['port']);
    }
    // Character set is added to dsn to ensure PDO uses the proper character
    // set when escaping. This has security implications. See
    // https://www.drupal.org/node/1201452 for further discussion.
    $dsn .= ';charset=' . $charset;
    $dsn .= ';dbname=' . $connection_options['database'];
    // Allow PDO options to be overridden.
    $connection_options += array(
      'pdo' => array(),
    );
    $connection_options['pdo'] += array(
      // Make sure MySQL returns all matched rows on update queries including
      // rows that actually didn't have to be updated because the values didn't
      // change. This matches common behaviour among other database systems.
      PDO::MYSQL_ATTR_FOUND_ROWS => TRUE,
      // So we don't have to mess around with cursors and unbuffered queries by default.
      PDO::MYSQL_ATTR_USE_BUFFERED_QUERY => TRUE,
      // Because MySQL's prepared statements skip the query cache, because it's dumb.
      PDO::ATTR_EMULATE_PREPARES => TRUE,
    );
    if (defined('PDO::MYSQL_ATTR_MULTI_STATEMENTS')) {
      // An added connection option in PHP 5.5.21+ to optionally limit SQL to a
      // single statement like mysqli.
      $connection_options['pdo'] += array(PDO::MYSQL_ATTR_MULTI_STATEMENTS => FALSE);
    }

    parent::__construct($dsn, $connection_options['username'], $connection_options['password'], $connection_options['pdo']);

    // Force MySQL to use the UTF-8 character set. Also set the collation, if a
    // certain one has been set; otherwise, MySQL defaults to 'utf8_general_ci'
    // for UTF-8.
    if (!empty($connection_options['collation'])) {
      $this->exec('SET NAMES ' . $charset . ' COLLATE ' . $connection_options['collation']);
    }
    else {
      $this->exec('SET NAMES ' . $charset);
    }

    // Set MySQL init_commands if not already defined.  Default Backdrop's MySQL
    // behavior to conform more closely to SQL standards.  This allows Backdrop
    // to run almost seamlessly on many different kinds of database systems.
    // Also, as MySQL 5.5 changed the meaning of TRADITIONAL we need to spell
    // out the modes one by one.
    $connection_options += array(
      'init_commands' => array(),
    );
    $connection_options['init_commands'] += array(
      'sql_mode' => "SET sql_mode = 'REAL_AS_FLOAT,PIPES_AS_CONCAT,ANSI_QUOTES,IGNORE_SPACE,STRICT_TRANS_TABLES,STRICT_ALL_TABLES,NO_ZERO_IN_DATE,NO_ZERO_DATE,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER'",
    );
    // Execute initial commands.
    foreach ($connection_options['init_commands'] as $sql) {
      $this->exec($sql);
    }
  }

  public function __destruct() {
    if ($this->needsCleanup) {
      $this->nextIdDelete();
    }
  }

  public function queryRange($query, $from, $count, array $args = array(), array $options = array()) {
    return $this->query($query . ' LIMIT ' . (int) $from . ', ' . (int) $count, $args, $options);
  }

  public function queryTemporary($query, array $args = array(), array $options = array()) {
    $tablename = $this->generateTemporaryTableName();
    $this->query('CREATE TEMPORARY TABLE {' . $tablename . '} Engine=MEMORY ' . $query, $args, $options);
    return $tablename;
  }

  public function driver() {
    return 'mysql';
  }

  public function databaseType() {
    return 'mysql';
  }

  public function mapConditionOperator($operator) {
    // We don't want to override any of the defaults.
    return NULL;
  }

  public function nextId($existing_id = 0) {
    $new_id = $this->query('INSERT INTO {sequences} () VALUES ()', array(), array('return' => Database::RETURN_INSERT_ID));
    // This should only happen after an import or similar event.
    if ($existing_id >= $new_id) {
      // If we INSERT a value manually into the sequences table, on the next
      // INSERT, MySQL will generate a larger value. However, there is no way
      // of knowing whether this value already exists in the table. MySQL
      // provides an INSERT IGNORE which would work, but that can mask problems
      // other than duplicate keys. Instead, we use INSERT ... ON DUPLICATE KEY
      // UPDATE in such a way that the UPDATE does not do anything. This way,
      // duplicate keys do not generate errors but everything else does.
      $this->query('INSERT INTO {sequences} (value) VALUES (:value) ON DUPLICATE KEY UPDATE value = value', array(':value' => $existing_id));
      $new_id = $this->query('INSERT INTO {sequences} () VALUES ()', array(), array('return' => Database::RETURN_INSERT_ID));
    }
    $this->needsCleanup = TRUE;
    return $new_id;
  }

  public function nextIdDelete() {
    // While we want to clean up the table to keep it up from occupying too
    // much storage and memory, we must keep the highest value in the table
    // because InnoDB  uses an in-memory auto-increment counter as long as the
    // server runs. When the server is stopped and restarted, InnoDB
    // reinitializes the counter for each table for the first INSERT to the
    // table based solely on values from the table so deleting all values would
    // be a problem in this case. Also, TRUNCATE resets the auto increment
    // counter.
    try {
      $max_id = $this->query('SELECT MAX(value) FROM {sequences}')->fetchField();
      // We know we are using MySQL here, no need for the slower db_delete().
      $this->query('DELETE FROM {sequences} WHERE value < :value', array(':value' => $max_id));
    }
    // During testing, this function is called from shutdown with the
    // simpletest prefix stored in $this->connection, and those tables are gone
    // by the time shutdown is called so we need to ignore the database
    // errors. There is no problem with completely ignoring errors here: if
    // these queries fail, the sequence will work just fine, just use a bit
    // more database storage and memory.
    catch (PDOException $e) {
    }
  }

  /**
   * Overridden to work around issues to MySQL not supporting transactional DDL.
   */
  protected function popCommittableTransactions() {
    // Commit all the committable layers.
    foreach (array_reverse($this->transactionLayers) as $name => $active) {
      // Stop once we found an active transaction.
      if ($active) {
        break;
      }

      // If there are no more layers left then we should commit.
      unset($this->transactionLayers[$name]);
      if (empty($this->transactionLayers)) {
        if (!PDO::commit()) {
          throw new DatabaseTransactionCommitFailedException();
        }
      }
      else {
        // Attempt to release this savepoint in the standard way.
        try {
          $this->query('RELEASE SAVEPOINT ' . $name);
        }
        catch (PDOException $e) {
          // However, in MySQL (InnoDB), savepoints are automatically committed
          // when tables are altered or created (DDL transactions are not
          // supported). This can cause exceptions due to trying to release
          // savepoints which no longer exist.
          //
          // To avoid exceptions when no actual error has occurred, we silently
          // succeed for MySQL error code 1305 ("SAVEPOINT does not exist").
          if ($e->errorInfo[1] == '1305') {
            // If one SAVEPOINT was released automatically, then all were.
            // Therefore, clean the transaction stack.
            $this->transactionLayers = array();
            // We also have to explain to PDO that the transaction stack has
            // been cleaned-up.
            PDO::commit();
          }
          else {
            throw $e;
          }
        }
      }
    }
  }

  public function utf8mb4IsActive() {
    return isset($this->connectionOptions['charset']) && $this->connectionOptions['charset'] === 'utf8mb4';
  }

  public function utf8mb4IsSupported() {
    // Caching if this method is called multiple times in the same request.
    if (isset($this->utf8mb4Supported)) {
      return $this->utf8mb4Supported;
    }

    // Ensure that the MySQL driver supports utf8mb4 encoding.
    $version = $this->getAttribute(PDO::ATTR_CLIENT_VERSION);

    if (strpos($version, 'mysqlnd') !== FALSE) {
      // The mysqlnd driver supports utf8mb4 starting at version 5.0.9.
      $version = preg_replace('/^\D+([\d.]+).*/', '$1', $version);
      if (version_compare($version, '5.0.9', '<')) {
        return FALSE;
      }
    }
    else {
      // The libmysqlclient driver supports utf8mb4 starting at version 5.5.3.
      if (version_compare($version, '5.5.3', '<')) {
        return FALSE;
      }
    }

    // Ensure that the MySQL server supports large prefixes and utf8mb4.
    try {
      $this->query("DROP TABLE IF EXISTS {backdrop_utf8mb4_test}");
      $this->query("CREATE TABLE {backdrop_utf8mb4_test} (id VARCHAR(255), PRIMARY KEY(id(255))) CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci ROW_FORMAT=DYNAMIC ENGINE=INNODB");
    }
    catch (Exception $e) {
      return FALSE;
    }
    $this->query("DROP TABLE {backdrop_utf8mb4_test}");
    return TRUE;
  }

  public function setSessionTimezoneOffset($offset) {
    $this->query("SET @@session.time_zone = :offset", array(':offset' => $offset));
  }


  public function hasTimezoneSupport() {
    $test = db_query("SELECT CONVERT_TZ('2008-02-15 12:00:00', 'UTC', 'US/Central')")->fetchField();
    return ($test == '2008-02-15 06:00:00');
  }

  public function dateFieldSql($field, $date_type = DATE_UNIX) {
    switch ($date_type) {
      case 'int':
        watchdog('database', 'Date field type "int" has been deprecated, use DATE_UNIX instead.', array(), WATCHDOG_DEPRECATED);
      case DATE_UNIX:
        $field = "FROM_UNIXTIME($field)";
        break;

      case DATE_ISO:
        $field = "STR_TO_DATE($field, '%Y-%m-%dT%T')";
        break;

      case DATE_DATETIME:
        break;
    }

    return $field;
  }

  public function dateMathSql($field, $direction, $count, $granularity) {
    $granularity = strtoupper($granularity);
    switch ($direction) {
      case 'ADD':
        return "DATE_ADD($field, INTERVAL $count $granularity)";

      case 'SUB':
        return "DATE_SUB($field, INTERVAL $count $granularity)";
    }
    return $field;
  }

  public function dateFormatSql($field, $format) {
    $replace = array(
      'Y' => '%Y',
      'y' => '%y',
      'M' => '%b',
      'm' => '%m',
      'n' => '%c',
      'F' => '%M',
      'D' => '%a',
      'd' => '%d',
      'l' => '%W',
      'j' => '%e',
      'W' => '%v',
      'H' => '%H',
      'h' => '%h',
      'i' => '%i',
      's' => '%s',
      'A' => '%p',
    );
    $format = strtr($format, $replace);
    return "DATE_FORMAT($field, '$format')";
  }

  public function dateExtractSql($field, $extract_type) {
    // Note there is no space after FROM to avoid db_rewrite problems
    // see http://drupal.org/node/79904.
    switch (strtoupper($extract_type)) {
      case 'DATE':
        return $field;
      case 'YEAR':
        return "EXTRACT(YEAR FROM($field))";
      case 'MONTH':
        return "EXTRACT(MONTH FROM($field))";
      case 'DAY':
        return "EXTRACT(DAY FROM($field))";
      case 'HOUR':
        return "EXTRACT(HOUR FROM($field))";
      case 'MINUTE':
        return "EXTRACT(MINUTE FROM($field))";
      case 'SECOND':
        return "EXTRACT(SECOND FROM($field))";

      // ISO week number for date.
      case 'WEEK':
        return "WEEK($field, 3)";
      case 'DOW':
        // MySQL returns 1 for Sunday through 7 for Saturday, PHP date
        // functions use 0 for Sunday and 6 for Saturday.
        return "INTEGER(DAYOFWEEK($field) - 1)";
      case 'DOY':
        return "DAYOFYEAR($field)";
    }

    // An unknown function.
    return FALSE;

  }

  public function dateConvertTimezoneSql($field, $source, $target) {
    return "CONVERT_TZ($field, $source, $target)";
  }
}


/**
 * @} End of "addtogroup database".
 */
