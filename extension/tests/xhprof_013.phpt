--TEST--
XHProf: Eval profiling test
Author: Evgeniy Snetilov
--FILE--
<?php

include_once dirname(__FILE__).'/common.php';

xhprof_enable();
eval('strcmp("aaa", "bbb");');
$output = xhprof_disable();

echo "Part 1: Simple eval\n";
print_canonical($output);
echo "\n";

?>
--EXPECTF--
3
