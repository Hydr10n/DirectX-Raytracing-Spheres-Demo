#pragma once

#pragma warning(push)
#pragma warning(disable: 4996 26451 26495 26812 33010)

#include "PxPhysicsAPI.h"

#include <stdexcept>

#include <numbers>
#include <cmath>

namespace PhysicsHelpers {
	namespace UniversalGravitation {
		constexpr auto G = 6.674e-11;

		template <std::floating_point T>
		constexpr T CalculateMass(T r, T t) { return static_cast<T>(4 * std::numbers::pi * std::numbers::pi * r * r * r / (G * t * t)); }

		template <std::floating_point T>
		constexpr T CalculateAccelerationMagnitude(T m, T r) { return static_cast<T>(G * m / (r * r)); }

		template <std::floating_point T>
		constexpr T CalculateFirstCosmicSpeed(T m, T r) { return static_cast<T>(sqrt(G * m / r)); }
	}

	namespace SimpleHarmonicMotion::Spring {
		template <std::floating_point T>
		constexpr T CalculateConstant(T m, T t) { return static_cast<T>(4 * std::numbers::pi * std::numbers::pi * m / (t * t)); }

		template <std::floating_point T>
		constexpr T CalculateDisplacement(T a, T omega, T t, T phi) { return a * cos(omega * t - phi); }

		template <std::floating_point T>
		constexpr T CalculateVelocity(T a, T omega, T t, T phi) { return -a * omega * sin(omega * t - phi); }
	}
}

#pragma warning(pop)

struct PhysX {
	PhysX(const PhysX&) = delete;
	PhysX& operator=(const PhysX&) = delete;

	PhysX(physx::PxU32 threadCount = 8) noexcept(false) {
		using namespace physx;

		auto& foundation = *_.Foundation;

		m_defaultCpuDispatcher = PxDefaultCpuDispatcherCreate(threadCount);

		PxTolerancesScale tolerancesScale;
		tolerancesScale.speed = 3;

		m_physics = PxCreatePhysics(PX_PHYSICS_VERSION, foundation, tolerancesScale);

		PxSceneDesc sceneDesc(tolerancesScale);
		sceneDesc.cpuDispatcher = m_defaultCpuDispatcher;
		sceneDesc.filterShader = PxDefaultSimulationFilterShader;

		m_scene = m_physics->createScene(sceneDesc);

		if (const auto scenePvdClient = m_scene->getScenePvdClient()) {
			scenePvdClient->setScenePvdFlags(PxPvdSceneFlag::eTRANSMIT_CONSTRAINTS | PxPvdSceneFlag::eTRANSMIT_CONTACTS | PxPvdSceneFlag::eTRANSMIT_SCENEQUERIES);
		}
	}

	~PhysX() {
		m_physics->release();

		m_defaultCpuDispatcher->release();
	}

	auto& GetPhysics() const noexcept { return *m_physics; }

	auto& GetScene() const noexcept { return *m_scene; }

	void Tick(physx::PxReal elapsedTime, bool block = true) {
		m_scene->simulate(elapsedTime);
		m_scene->fetchResults(block);
	}

private:
	inline static const struct _ {
		struct PxAllocator : physx::PxDefaultAllocator {
			void* allocate(size_t size, const char* typeName, const char* filename, int line) override {
				void* ptr = PxDefaultAllocator::allocate(size, typeName, filename, line);
				if (ptr == nullptr) throw std::bad_alloc();
				return ptr;
			}
		} AllocatorCallback;

		physx::PxDefaultErrorCallback ErrorCallback;

		physx::PxFoundation* Foundation = PxCreateFoundation(PX_PHYSICS_VERSION, AllocatorCallback, ErrorCallback);

		~_() { Foundation->release(); }
	} _;

	physx::PxDefaultCpuDispatcher* m_defaultCpuDispatcher{};

	physx::PxPhysics* m_physics{};

	physx::PxScene* m_scene{};
};
