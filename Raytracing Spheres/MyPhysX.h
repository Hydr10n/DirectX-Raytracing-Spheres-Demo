#pragma once

#include "PxPhysicsAPI.h"

#include <stdexcept>

#include <numbers>
#include <cmath>

namespace PhysicsHelpers {
	namespace Gravity {
		constexpr auto G = 6.674e-11;

		template <class T>
		constexpr T CalculateMass(T r, T t) { return static_cast<T>(4 * std::numbers::pi * std::numbers::pi * r * r * r / (G * t * t)); }

		template <class T>
		constexpr T CalculateAccelerationMagnitude(T m, T r) { return static_cast<T>(G * m / (r * r)); }

		template <class T>
		constexpr T CalculateFirstCosmicSpeed(T m, T r) { return static_cast<T>(sqrt(G * m / r)); }
	}

	namespace SimpleHarmonicMotion {
		template <class T>
		constexpr T CalculateSpringConstant(T m, T t) { return static_cast<T>(4 * std::numbers::pi * std::numbers::pi * m / (t * t)); }

		template <class T>
		constexpr T CalculateSpringDisplacement(T a, T ω, T t, T φ) { return a * cos(ω * t + φ); }

		template <class T>
		constexpr T CalculateSpringSpeed(T a, T ω, T t, T φ) { return -a * ω * sin(ω * t + φ); }
	}
}

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

		m_material = m_physics->createMaterial(0.5f, 0.5f, 0.6f);

		PxSceneDesc sceneDesc(tolerancesScale);
		sceneDesc.cpuDispatcher = m_defaultCpuDispatcher;
		sceneDesc.filterShader = PxDefaultSimulationFilterShader;

		m_scene = m_physics->createScene(sceneDesc);

		const auto scenePvdClient = m_scene->getScenePvdClient();
		if (scenePvdClient != nullptr) {
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

	physx::PxPhysics& GetPhysics() const { return *m_physics; }

	void Tick(physx::PxReal elapsedTime, bool block = true) {
		m_scene->simulate(elapsedTime);
		m_scene->fetchResults(block);
	}

	physx::PxRigidDynamic* AddRigidDynamic(const physx::PxGeometry& geometry, const physx::PxTransform& transform) {
		const auto rigidDynamic = m_physics->createRigidDynamic(transform);
		physx::PxRigidActorExt::createExclusiveShape(*rigidDynamic, geometry, *m_material);
		m_scene->addActor(*rigidDynamic);
		return rigidDynamic;
	}

private:
	struct PxAllocator : physx::PxDefaultAllocator {
		void* allocate(size_t size, const char* typeName, const char* filename, int line) override {
			void* ptr = physx::PxDefaultAllocator::allocate(size, typeName, filename, line);
			if (ptr == nullptr) throw std::bad_alloc();
			return ptr;
		}
	} m_allocatorCallback;

	physx::PxDefaultErrorCallback m_errorCallback;

	physx::PxPvd* m_pvd{};

	physx::PxPhysics* m_physics{};

	physx::PxDefaultCpuDispatcher* m_defaultCpuDispatcher{};

	physx::PxMaterial* m_material{};

	physx::PxScene* m_scene{};
};
