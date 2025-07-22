#include "game.h"
#include "../system/app.h"
#include "../system/memory.h"
#include "../system/thread_pool.h"
#include "../system/task.h"

#include <SDL3/SDL_log.h>

typedef struct
{
	float x, y, z;
	float vx, vy, vz;
	float health;
	uint32_t unit_type;
	uint32_t player_id;
} Unit;

typedef struct
{
	Unit* units;
	uint32_t unit_count;
	uint32_t capacity;
} GameWorld;

// Example task data structures
typedef struct
{
	Unit* units;
	uint32_t start_index;
	uint32_t count;
	float delta_time;
} MovementTaskData;

typedef struct
{
	Unit* units;
	uint32_t start_index;
	uint32_t count;
	GameWorld* world;
} AITaskData;

// AI decision making task
void process_ai_decisions(void* data)
{
	AITaskData* ai_data = (AITaskData*)data;

	SDL_Log("AI Task: Processing %u units starting at %u on worker %u\n", ai_data->count, ai_data->start_index,
		   wc_task_get_worker_id());

	for (uint32_t i = 0; i < ai_data->count; i++)
	{
		Unit* unit = &ai_data->units[ai_data->start_index + i];

		// Simple AI: move towards center if health is good
		if (unit->health > 50.0f)
		{
			float dx = 0.0f - unit->x;
			float dy = 0.0f - unit->y;
			float distance = sqrtf(dx * dx + dy * dy);

			if (distance > 1.0f)
			{
				unit->vx = (dx / distance) * 10.0f;
				unit->vy = (dy / distance) * 10.0f;
			}
		}
	}
}

// Movement processing task
void process_movement(void* data)
{
	MovementTaskData* move_data = (MovementTaskData*)data;

	SDL_Log("Movement Task: Processing %u units starting at %u on worker %u\n", move_data->count, move_data->start_index,
		   wc_task_get_worker_id());

	for (uint32_t i = 0; i < move_data->count; i++)
	{
		Unit* unit = &move_data->units[move_data->start_index + i];

		// Update position based on velocity
		unit->x += unit->vx * move_data->delta_time;
		unit->y += unit->vy * move_data->delta_time;
		unit->z += unit->vz * move_data->delta_time;

		// Simple boundary checking
		if (unit->x < -100.0f)
			unit->x = -100.0f;
		if (unit->x > 100.0f)
			unit->x = 100.0f;
		if (unit->y < -100.0f)
			unit->y = -100.0f;
		if (unit->y > 100.0f)
			unit->y = 100.0f;
	}
}

// Combat resolution task
void process_combat(void* data)
{
	MovementTaskData* combat_data = (MovementTaskData*)data;

	SDL_Log("Combat Task: Processing %u units starting at %u on worker %u\n", combat_data->count, combat_data->start_index,
		   wc_task_get_worker_id());

	// Simple combat: reduce health over time
	for (uint32_t i = 0; i < combat_data->count; i++)
	{
		Unit* unit = &combat_data->units[combat_data->start_index + i];
		unit->health -= 1.0f * combat_data->delta_time;
		if (unit->health < 0.0f)
			unit->health = 0.0f;
	}
}

// Cooperative task example
WC_TaskYield process_large_dataset(void* data)
{
	static uint32_t processed_items = 0;
	uint32_t* total_items = (uint32_t*)data;

	const uint32_t items_per_yield = 1000;
	uint32_t items_this_round = 0;

	while (processed_items < *total_items && items_this_round < items_per_yield)
	{
		// Simulate work
		processed_items++;
		items_this_round++;

		// Some expensive computation
		volatile float result = sinf((float)processed_items);
		(void)result; // Prevent optimization
	}

	SDL_Log("Cooperative task processed %u/%u items on worker %u\n", processed_items, *total_items, wc_task_get_worker_id());

	if (processed_items >= *total_items)
	{
		processed_items = 0; // Reset for next time
		return WC_TASK_COMPLETE;
	}

	return WC_TASK_YIELD; // Yield control and reschedule
}

//-------------------------------------------------------------------------------------------------
// Task system integration with game loop
//-------------------------------------------------------------------------------------------------

void wc_game_frame_with_tasks(GameWorld* world, float delta_time)
{
	const uint32_t units_per_task = 256;
	const uint32_t total_units = world->unit_count;
	const uint32_t num_tasks = (total_units + units_per_task - 1) / units_per_task;

	SDL_Log("\n=== Processing frame with %u units using %u tasks ===\n", total_units, num_tasks);

	// Create task group for this frame
	WC_TaskGroup* frame_group = wc_task_group_create(num_tasks * 3); // AI + Movement + Combat

	// Arrays to hold our tasks
	WC_Task** ai_tasks = wc_malloc(num_tasks * sizeof(WC_Task*));
	WC_Task** movement_tasks = wc_malloc(num_tasks * sizeof(WC_Task*));
	WC_Task** combat_tasks = wc_malloc(num_tasks * sizeof(WC_Task*));

	// Task data arrays (allocated from frame arena)
	WC_Arena* group_arena = wc_task_group_get_arena(frame_group);
	AITaskData* ai_data = wc_arena_alloc(group_arena, num_tasks * sizeof(AITaskData));
	MovementTaskData* movement_data = wc_arena_alloc(group_arena, num_tasks * sizeof(MovementTaskData));
	MovementTaskData* combat_data = wc_arena_alloc(group_arena, num_tasks * sizeof(MovementTaskData));

	// Phase 1: Create AI tasks (no dependencies)
	for (uint32_t i = 0; i < num_tasks; i++)
	{
		uint32_t start_index = i * units_per_task;
		uint32_t count = (start_index + units_per_task > total_units) ? total_units - start_index : units_per_task;

		// Setup AI task data
		ai_data[i].units = world->units;
		ai_data[i].start_index = start_index;
		ai_data[i].count = count;
		ai_data[i].world = world;

		// Create AI task
		ai_tasks[i] = wc_task_create(process_ai_decisions, &ai_data[i]);
		wc_task_group_add(frame_group, ai_tasks[i]);
	}

	// Phase 2: Create movement tasks (depend on AI)
	for (uint32_t i = 0; i < num_tasks; i++)
	{
		uint32_t start_index = i * units_per_task;
		uint32_t count = (start_index + units_per_task > total_units) ? total_units - start_index : units_per_task;

		// Setup movement task data
		movement_data[i].units = world->units;
		movement_data[i].start_index = start_index;
		movement_data[i].count = count;
		movement_data[i].delta_time = delta_time;

		// Create movement task
		movement_tasks[i] = wc_task_create(process_movement, &movement_data[i]);
		wc_task_group_add(frame_group, movement_tasks[i]);

		// Movement depends on corresponding AI task
		wc_task_add_dependency(movement_tasks[i], ai_tasks[i]);
	}

	// Phase 3: Create combat tasks (depend on movement)
	for (uint32_t i = 0; i < num_tasks; i++)
	{
		uint32_t start_index = i * units_per_task;
		uint32_t count = (start_index + units_per_task > total_units) ? total_units - start_index : units_per_task;

		// Setup combat task data
		combat_data[i].units = world->units;
		combat_data[i].start_index = start_index;
		combat_data[i].count = count;
		combat_data[i].delta_time = delta_time;

		// Create combat task
		combat_tasks[i] = wc_task_create(process_combat, &combat_data[i]);
		wc_task_group_add(frame_group, combat_tasks[i]);

		// Combat depends on corresponding movement task
		wc_task_add_dependency(combat_tasks[i], movement_tasks[i]);
	}

	// Submit all AI tasks first (they have no dependencies)
	wc_task_submit_batch(ai_tasks, num_tasks);

	// Submit movement and combat tasks (they'll run when dependencies are met)
	wc_task_submit_batch(movement_tasks, num_tasks);
	wc_task_submit_batch(combat_tasks, num_tasks);

	// Wait for all tasks in the frame to complete
	SDL_Log("Waiting for frame tasks to complete...\n");
	wc_task_group_wait(frame_group);
	SDL_Log("Frame processing complete!\n");

	// Cleanup
	wc_free(ai_tasks);
	wc_free(movement_tasks);
	wc_free(combat_tasks);
	wc_task_group_destroy(frame_group);
}

//-------------------------------------------------------------------------------------------------
// Example usage and integration
//-------------------------------------------------------------------------------------------------

int example_task_system_usage(void)
{
	SDL_Log("Initializing task system...\n");

	// Initialize global task system
	if (wc_init_global_pool() != 0)
	{
		SDL_Log("Failed to initialize task system!\n");
		return -1;
	}

	// Create example game world
	const uint32_t unit_count = 10000;
	GameWorld world = {0};
	world.capacity = unit_count;
	world.unit_count = unit_count;
	world.units = wc_malloc(unit_count * sizeof(Unit));

	// Initialize units with random positions
	for (uint32_t i = 0; i < unit_count; i++)
	{
		world.units[i].x = (float)(SDL_rand(200) - 100);
		world.units[i].y = (float)(SDL_rand(200) - 100);
		world.units[i].z = 0.0f;
		world.units[i].vx = 0.0f;
		world.units[i].vy = 0.0f;
		world.units[i].vz = 0.0f;
		world.units[i].health = 100.0f;
		world.units[i].unit_type = SDL_rand(3);
		world.units[i].player_id = SDL_rand(4);
	}

	// Example: Process several game frames
	for (int frame = 0; frame < 3; frame++)
	{
		SDL_Log("\n--- Frame %d ---\n", frame);
		wc_game_frame_with_tasks(&world, 1.0f / 60.0f);
	}

	// Example: Cooperative task
	SDL_Log("\n--- Cooperative Task Example ---\n");
	uint32_t large_dataset_size = 50000;
	WC_Task* coop_task = wc_task_create_cooperative(process_large_dataset, &large_dataset_size);
	wc_task_submit(coop_task);
	wc_task_wait(coop_task);

	// Print statistics
	SDL_Log("\n--- Task System Statistics ---\n");
	WC_PoolStats pool_stats = wc_pool_get_stats(wc_get_global_pool());
	SDL_Log("Tasks submitted: %llu\n", (unsigned long long)pool_stats.total_tasks_submitted);
	SDL_Log("Tasks completed: %llu\n", (unsigned long long)pool_stats.total_tasks_completed);
	SDL_Log("Overall steal success rate: %.2f%%\n", pool_stats.overall_steal_success_rate * 100.0);
	SDL_Log("Worker utilization: %.2f%%\n", pool_stats.overall_utilization * 100.0);

	// Print per-worker statistics
	WC_LoadBalanceStats* worker_stats = wc_malloc(pool_stats.worker_count * sizeof(WC_LoadBalanceStats));
	wc_pool_get_load_stats(wc_get_global_pool(), worker_stats);

	SDL_Log("\nPer-worker statistics:\n");
	for (uint32_t i = 0; i < pool_stats.worker_count; i++)
	{
		SDL_Log("Worker %u: %u tasks, %.1f%% utilization, %.1f%% steal success\n", worker_stats[i].worker_id,
			   worker_stats[i].tasks_executed, worker_stats[i].utilization * 100.0, worker_stats[i].steal_success_rate * 100.0);
	}

	// Cleanup
	wc_free(worker_stats);
	wc_free(world.units);
	wc_shutdown_global_pool();

	SDL_Log("\nTask system example completed successfully!\n");
	return 0;
}

int wc_game_init()
{
	return example_task_system_usage();
}

void wc_game_update(const double delta_time)
{
}

void wc_game_render(const double interpolant)
{
}

void wc_game_quit()
{
}
