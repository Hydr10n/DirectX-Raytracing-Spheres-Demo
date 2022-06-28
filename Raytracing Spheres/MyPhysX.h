#pragma once

#pragma warning(push)
#pragma warning(disable: 26451 26495 26812 33010)

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
		constexpr T CalculateDisplacement(T a, T ω, T t, T φ) { return a * cos(ω * t - φ); }

		template <std::floating_point T>
		constexpr T CalculateVelocity(T a, T ω, T t, T φ) { return -a * ω * sin(ω * t - φ); }
	}
}

#pragma warning(pop)

class MyPhysX {
public:
	MyPhysX(const MyPhysX&) = delete;
	MyPhysX& operator=(const MyPhysX&) = delete;

	MyPhysX() noexcept(false) {
		using namespace physx;

		const auto foundation = PxCreateFoundation(PX_PHYSICS_VERSION, m_allocatorCallback, m_errorCallback);

		m_pvd = PxCreatePvd(*foundation);
		m_pvd->connect(*PxDefaultPvdSocketTransportCreate("localhost", 5425, 10), PxPvdInstrumentationFlag::eALL);

		PxTolerancesScale tolerancesScale;
		tolerancesScale.speed = 3;

		m_physics = PxCreatePhysics(PX_PHYSICS_VERSION, *foundation, tolerancesScale, true, m_pvd);

		m_defaultCpuDispatcher = PxDefaultCpuDispatcherCreate(8);

		PxSceneDesc sceneDesc(tolerancesScale);
		sceneDesc.cpuDispatcher = m_defaultCpuDispatcher;
		sceneDesc.filterShader = PxDefaultSimulationFilterShader;

		m_scene = m_physics->createScene(sceneDesc);

		if (const auto scenePvdClient = m_scene->getScenePvdClient()) {
			scenePvdClient->setScenePvdFlags(PxPvdSceneFlag::eTRANSMIT_CONSTRAINTS | PxPvdSceneFlag::eTRANSMIT_CONTACTS | PxPvdSceneFlag::eTRANSMIT_SCENEQUERIES);
		}
	}

	~MyPhysX() {
		m_defaultCpuDispatcher->release();

		auto& foundation = m_physics->getFoundation();

		m_physics->release();

		m_pvd->disconnect();
		m_pvd->getTransport()->release();
		m_pvd->release();

		foundation.release();
	}

	auto& GetPhysics() const noexcept { return *m_physics; }

	auto& GetScene() const noexcept { return *m_scene; }

	void Tick(physx::PxReal elapsedTime, bool block = true) {
		m_scene->simulate(elapsedTime);
		m_scene->fetchResults(block);
	}

private:
	struct PxAllocator : physx::PxDefaultAllocator {
		void* allocate(size_t size, const char* typeName, const char* filename, int line) override {
			void* ptr = PxDefaultAllocator::allocate(size, typeName, filename, line);
			if (ptr == nullptr) throw std::bad_alloc();
			return ptr;
		}
	} m_allocatorCallback;

	physx::PxDefaultErrorCallback m_errorCallback;

	physx::PxPvd* m_pvd{};

	physx::PxPhysics* m_physics{};

	physx::PxDefaultCpuDispatcher* m_defaultCpuDispatcher{};

	physx::PxScene* m_scene{};
};
