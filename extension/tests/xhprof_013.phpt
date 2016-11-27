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
Part 1: Simple eval
main()                                  : ct=       1; wt=*;
main()==>eval::tests/xhprof_013.php(6) : eval()'d code: ct=       1; wt=*;
main()==>run_init::tests/xhprof_013.php(6) : eval()'d code: ct=       1; wt=*;
main()==>xhprof_disable                 : ct=       1; wt=*;
run_init::tests/xhprof_013.php(6) : eval()'d code==>strcmp: ct=       1; wt=*;