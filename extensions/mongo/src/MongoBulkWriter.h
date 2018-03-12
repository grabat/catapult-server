#pragma once
#include "catapult/model/Elements.h"
#include "catapult/state/AccountState.h"
#include "catapult/thread/FutureUtils.h"
#include "catapult/thread/IoServiceThreadPool.h"
#include "catapult/thread/ParallelFor.h"
#include "catapult/utils/MemoryUtils.h"
#include "catapult/exceptions.h"
#include "catapult/types.h"
#include <boost/asio/io_service.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>
#ifndef _MSC_VER
#pragma GCC visibility push(default)
#endif
#include <mongocxx/exception/bulk_write_exception.hpp>
#ifndef _MSC_VER
#pragma GCC visibility pop
#endif
#include <mongocxx/pool.hpp>
#include <unordered_set>

namespace catapult { namespace mongo {

	/// Result of a bulk write operation to the database.
	struct BulkWriteResult {
	public:
		/// Creates a default bulk write result.
		BulkWriteResult()
				: NumInserted(0)
				, NumMatched(0)
				, NumModified(0)
				, NumDeleted(0)
				, NumUpserted(0)
		{}

		/// Creates a bulk result from a mongo \a result.
		explicit BulkWriteResult(const mongocxx::result::bulk_write& result)
				: NumInserted(result.inserted_count())
				, NumMatched(result.matched_count())
				, NumModified(result.modified_count())
				, NumDeleted(result.deleted_count())
				, NumUpserted(result.upserted_count())
		{}

	public:
		/// Aggregates all bulk write results in \a results into a single result.
		static BulkWriteResult Aggregate(const std::vector<BulkWriteResult>& results) {
			BulkWriteResult aggregate;
			for (const auto& result : results) {
				aggregate.NumInserted += result.NumInserted;
				aggregate.NumMatched += result.NumMatched;
				aggregate.NumModified += result.NumModified;
				aggregate.NumDeleted += result.NumDeleted;
				aggregate.NumUpserted += result.NumUpserted;
			}

			return aggregate;
		}

	public:
		/// The number of documents that were inserted.
		int32_t NumInserted;

		/// The number of documents that matched existing documents.
		int32_t NumMatched;

		/// The number of existing documents that were modified.
		int32_t NumModified;

		/// The number of existing documents that were deleted.
		int32_t NumDeleted;

		/// The number of documents that were inserted because no document matched.
		int32_t NumUpserted;
	};

	/// Class for writing bulk data to the mongo database.
	/// The bulk writer supports inserting, upserting and deleting documents.
	class MongoBulkWriter final : public std::enable_shared_from_this<MongoBulkWriter> {
	private:
		using AccountStates = std::unordered_set<std::shared_ptr<const state::AccountState>>;
		using BulkWriteResultFuture = thread::future<std::vector<thread::future<BulkWriteResult>>>;

		template<typename TEntity>
		using AppendOperation = consumer<mongocxx::bulk_write&, const TEntity&, uint32_t>;

		template<typename TEntity>
		using CreateDocument = std::function<bsoncxx::document::value (const TEntity&, uint32_t)>;

		template<typename TEntity>
		using CreateDocuments = std::function<std::vector<bsoncxx::document::value> (const TEntity&, uint32_t)>;

		template<typename TEntity>
		using CreateFilter = std::function<bsoncxx::document::value (const TEntity&)>;

	private:
		MongoBulkWriter(const mongocxx::uri& uri, const std::string& dbName, const std::shared_ptr<thread::IoServiceThreadPool>& pPool)
				: m_dbName(dbName)
				, m_pPool(pPool)
				, m_service(pPool->service())
				, m_connectionPool(uri)
		{}

	public:
		/// Creates a mongo bulk writer connected to \a uri that will use database \a dbName for bulk writes.
		/// The data is written concurrently using the specified thread pool (\a pPool).
		static std::shared_ptr<MongoBulkWriter> Create(
				const mongocxx::uri& uri,
				const std::string& dbName,
				const std::shared_ptr<thread::IoServiceThreadPool>& pPool) {
			// cannot use make_shared with private constructor
			auto pData = utils::MakeUniqueWithSize<uint8_t>(sizeof(MongoBulkWriter));
			auto pWriterRaw = new (pData.get()) MongoBulkWriter(uri, dbName, pPool);
			auto pWriter = std::shared_ptr<MongoBulkWriter>(pWriterRaw);
			pData.release();
			return pWriter;
		}

	public:
		/// Inserts \a entities into the collection named \a collectionName using a one-to-one mapping of entities
		/// to documents (\a createDocument).
		template<typename TContainer>
		BulkWriteResultFuture bulkInsert(
				const std::string& collectionName,
				const TContainer& entities,
				const CreateDocument<typename TContainer::value_type>& createDocument) {
			auto appendOperation = [createDocument](auto& bulk, const auto& entity, auto index) {
				auto entityDocument = createDocument(entity, index);
				bulk.append(mongocxx::model::insert_one(entityDocument.view()));
			};

			return bulkWrite<TContainer>(collectionName, entities, appendOperation);
		}

		/// Inserts \a entities into the collection named \a collectionName using a one-to-many mapping of entities
		/// to documents (\a createDocuments).
		template<typename TContainer>
		BulkWriteResultFuture bulkInsert(
				const std::string& collectionName,
				const TContainer& entities,
				const CreateDocuments<typename TContainer::value_type>& createDocuments) {
			auto appendOperation = [createDocuments](auto& bulk, const auto& entity, auto index) {
				for (const auto& entityDocument : createDocuments(entity, index))
					bulk.append(mongocxx::model::insert_one(entityDocument.view()));
			};

			return bulkWrite<TContainer>(collectionName, entities, appendOperation);
		}

		/// Upserts \a entities into the collection named \a collectionName using a one-to-one mapping of entities
		/// to documents (\a createDocument) matching the specified entity filter (\a createFilter).
		template<typename TContainer>
		BulkWriteResultFuture bulkUpsert(
				const std::string& collectionName,
				const TContainer& entities,
				const CreateDocument<typename TContainer::value_type>& createDocument,
				const CreateFilter<typename TContainer::value_type>& createFilter) {
			auto appendOperation = [createDocument, createFilter](auto& bulk, const auto& entity, auto index) {
				auto entityDocument = createDocument(entity, index);
				auto filter = createFilter(entity);
				mongocxx::model::replace_one replace_op(filter.view(), entityDocument.view());
				replace_op.upsert(true);
				bulk.append(replace_op);
			};

			return bulkWrite<TContainer>(collectionName, entities, appendOperation);
		}

		/// Deletes \a entities from the collection named \a collectionName matching the specified entity filter (\a createFilter).
		template<typename TContainer>
		BulkWriteResultFuture bulkDelete(
				const std::string& collectionName,
				const TContainer& entities,
				const CreateFilter<typename TContainer::value_type>& createFilter) {
			auto appendOperation = [createFilter](auto& bulk, const auto& entity, auto) {
				auto filter = createFilter(entity);
				bulk.append(mongocxx::model::delete_many(filter.view()));
			};

			return bulkWrite<TContainer>(collectionName, entities, appendOperation);
		}

	private:
		thread::future<BulkWriteResult> handleBulkOperation(
				const std::string& collectionName,
				const std::shared_ptr<mongocxx::bulk_write>& pBulk) {
			auto pPromise = std::make_shared<thread::promise<BulkWriteResult>>();
			m_service.post([pThis = shared_from_this(), collectionName, pBulk, pPromise]() {
				pThis->bulkWrite(collectionName, *pBulk, *pPromise);
			});

			return pPromise->get_future();
		}

		void bulkWrite(const std::string& collectionName, mongocxx::bulk_write& bulk, thread::promise<BulkWriteResult>& promise) {
			try {
				auto pConnection = m_connectionPool.acquire();
				auto database = pConnection->database(m_dbName);
				auto collection = database[collectionName];

				// if something goes wrong mongo will throw, else a result is always available
				auto result = collection.bulk_write(bulk).get();
				promise.set_value(BulkWriteResult(result));
			} catch (const mongocxx::bulk_write_exception& e) {
				std::ostringstream stream;
				stream << "message: " << e.code().message();
				if (e.raw_server_error()) {
					auto description = bsoncxx::to_json(e.raw_server_error().get());
					stream << ", description: " << description;
				}

				CATAPULT_LOG(fatal) << "throwing exception: " << stream.str().c_str();
				promise.set_exception(std::make_exception_ptr(catapult_runtime_error(stream.str().c_str())));
			}
		}

		struct BulkWriteContext {
		public:
			explicit BulkWriteContext(size_t numOperations) : m_futures(numOperations)
			{}

		public:
			BulkWriteResultFuture aggregateFuture() {
				return thread::when_all(std::move(m_futures));
			}

			void setFutureAt(size_t index, thread::future<BulkWriteResult>&& future) {
				m_futures[index] = std::move(future);
			}

		private:
			std::vector<thread::future<BulkWriteResult>> m_futures;
		};

		template<typename TEntity, typename TContainer>
		BulkWriteResultFuture bulkWrite(
				const std::string& collectionName,
				const TContainer& entities,
				const AppendOperation<typename TContainer::value_type>& appendOperation) {
			if (entities.empty())
				return thread::make_ready_future(std::vector<thread::future<BulkWriteResult>>());

			auto numThreads = m_pPool->numWorkerThreads();
			auto pContext = std::make_shared<BulkWriteContext>(std::min<size_t>(entities.size(), numThreads));
			return thread::compose(
					thread::ParallelForPartition(
							m_service,
							entities,
							numThreads,
							[pThis = shared_from_this(), entitiesStart = entities.cbegin(), collectionName, appendOperation, pContext](
									auto itBegin,
									auto itEnd,
									auto startIndex,
									auto batchIndex) {
								auto pBulk = std::make_shared<mongocxx::bulk_write>();
								auto index = static_cast<uint32_t>(startIndex);
								for (auto iter = itBegin; itEnd != iter; ++iter, ++index)
									appendOperation(*pBulk, *iter, index);

								pContext->setFutureAt(batchIndex, pThis->handleBulkOperation(collectionName, pBulk));
							}),
					[pContext](const auto&) {
						return pContext->aggregateFuture();
					});
		}

	private:
		std::string m_dbName;
		std::shared_ptr<const thread::IoServiceThreadPool> m_pPool;
		boost::asio::io_service& m_service;
		mongocxx::pool m_connectionPool;
	};
}}