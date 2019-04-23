<?php

namespace Concurrent;

$pool = new ThreadPool(2, __DIR__ . '/threads-bootstrap.php');

$job1 = $pool->submit(function () {
    sleep(1);
    return 'Hello';
});

var_dump('GO');
(new Timer(100))->awaitTimeout();
var_dump('NAU');
    
$job2 = $pool->submit(function () {
    return subject();
});

var_dump(Task::await($job1));
var_dump(Task::await($job2));

$pool->close();
