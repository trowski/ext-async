<?php

namespace Concurrent;

$pool = new ThreadPool(1, __DIR__ . '/thread-bootstrap.php');

$job = $pool->submit(function () {
    return 'Hello';
});

var_dump('GO');
(new Timer(100))->awaitTimeout();
var_dump('NAU');

var_dump($job);
var_dump(Task::await($job));

$pool->close();
