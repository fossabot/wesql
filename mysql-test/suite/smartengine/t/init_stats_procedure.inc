# This inc script creates two procedures -- save_read_stats() and
# get_read_stats(). get_read_stats() prints differential smartengine_rows_read,
# smartengine_rows_updated, and smartengine_rows_deleted values since calling
# save_read_stats().

delimiter //;
create procedure save_read_stats()
begin
  select rows_requested into @rq from information_schema.table_statistics
    where table_schema=database() and table_name='t1';
  select variable_value into @rr from information_schema.global_status
    where variable_name='smartengine_rows_read';
  select variable_value into @ru from information_schema.global_status
    where variable_name='smartengine_rows_updated';
  select variable_value into @rd from information_schema.global_status
    where variable_name='smartengine_rows_deleted';
end//

create procedure get_read_stats()
begin
  select rows_requested - @rq as rows_requested from
    information_schema.table_statistics
    where table_schema=database() and table_name='t1';
  select variable_value - @rr as rows_read from
    information_schema.global_status
    where variable_name='smartengine_rows_read';
  select variable_value - @ru as rows_updated from
    information_schema.global_status
    where variable_name='smartengine_rows_updated';
  select variable_value - @rd as rows_deleted from
    information_schema.global_status
    where variable_name='smartengine_rows_deleted';
end//
delimiter ;//

