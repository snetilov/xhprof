--TEST--
XHProf: Second simplest memory leaks test
Author: Evgeniy Snetilov
--FILE--
<?php

xhprof_enable();
xhprof_disable();

?>
--EXPECTF--

