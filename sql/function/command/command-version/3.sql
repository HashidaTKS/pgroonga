SELECT
   json_typeof(
     pgroonga_command('status',
                      ARRAY[
                        'command_version', '3'
                      ])::json
  ) AS envelope_type;
