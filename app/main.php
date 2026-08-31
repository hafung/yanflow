<?php

function main(): void
{
    global $argv;
    if (count($argv) > 1 && $argv[1] === '--asr-smoke') {
        $exitCode = yanflow_asr_smoke();
        if ($exitCode !== 0) {
            exit($exitCode);
        }
        return;
    }
    if (count($argv) > 1 && $argv[1] === '--e2e-smoke') {
        $exitCode = yanflow_run(-1);
        if ($exitCode !== 0) {
            exit($exitCode);
        }
        return;
    }
    if (count($argv) > 1 && $argv[1] === '--fallback-smoke') {
        $exitCode = yanflow_run(-2);
        if ($exitCode !== 0) {
            exit($exitCode);
        }
        return;
    }
    if (count($argv) > 1 && $argv[1] === '--capture-smoke') {
        $exitCode = yanflow_run(-3);
        if ($exitCode !== 0) {
            exit($exitCode);
        }
        return;
    }
    if (count($argv) > 1 && $argv[1] === '--pipeline-smoke') {
        $exitCode = yanflow_run(-4);
        if ($exitCode !== 0) {
            exit($exitCode);
        }
        return;
    }
    if (count($argv) > 1 && $argv[1] === '--readme-demo') {
        $exitCode = yanflow_run(-5);
        if ($exitCode !== 0) {
            exit($exitCode);
        }
        return;
    }
    $smokeMilliseconds = 0;
    if (count($argv) > 1 && $argv[1] === '--smoke') {
        $smokeMilliseconds = 800;
    }

    $exitCode = yanflow_run($smokeMilliseconds);
    if ($exitCode !== 0) {
        exit($exitCode);
    }
}
