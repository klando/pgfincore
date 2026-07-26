CREATE EXTENSION pgfincore VERSION "1.5";
ALTER EXTENSION pgfincore UPDATE TO "1.4";

-- those require version "1.5"
SELECT pg_page_size();
SELECT pg_segment_size();
SELECT vm_page_size();
SELECT FROM vm_physical_pages();
SELECT FROM vm_available_pages();

DROP EXTENSION pgfincore;
