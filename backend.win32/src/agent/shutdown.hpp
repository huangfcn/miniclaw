#pragma once

/**
 * @brief Triggers a graceful shutdown of the application.
 * 
 * This can be called from signal handlers or API endpoints.
 */
void miniclaw_trigger_shutdown();

/**
 * @brief Blocks the calling thread until a shutdown is triggered.
 */
void miniclaw_wait_for_shutdown();
