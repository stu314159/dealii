// ---------------------------------------------------------------------
//
// Copyright (C) 2018 - 2023 by the deal.II authors
//
// This file is part of the deal.II library.
//
// The deal.II library is free software; you can use it, redistribute
// it, and/or modify it under the terms of the GNU Lesser General
// Public License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
// The full text of the license can be found in the file LICENSE.md at
// the top level directory of deal.II.
//
// ---------------------------------------------------------------------

#ifndef dealii_trilinos_tpetra_vector_templates_h
#define dealii_trilinos_tpetra_vector_templates_h

#include <deal.II/base/config.h>

#include <deal.II/base/mpi.h>

#include <deal.II/lac/trilinos_tpetra_vector.h>

#ifdef DEAL_II_TRILINOS_WITH_TPETRA

#  include <deal.II/base/index_set.h>
#  include <deal.II/base/trilinos_utilities.h>

#  include <deal.II/lac/read_write_vector.h>

#  include <boost/io/ios_state.hpp>

#  include <Teuchos_DefaultMpiComm.hpp>
#  include <Tpetra_Import_def.hpp>
#  include <Tpetra_Map_def.hpp>

#  include <memory>


DEAL_II_NAMESPACE_OPEN

namespace LinearAlgebra
{
  namespace TpetraWrappers
  {
    template <typename Number>
    Vector<Number>::Vector()
      : Subscriptor()
      , vector(Utilities::Trilinos::internal::make_rcp<VectorType>(
          Utilities::Trilinos::internal::make_rcp<MapType>(
            0,
            0,
            Utilities::Trilinos::tpetra_comm_self())))
    {}



    template <typename Number>
    Vector<Number>::Vector(const Vector<Number> &V)
      : Subscriptor()
      , vector(Utilities::Trilinos::internal::make_rcp<VectorType>(
          V.trilinos_vector(),
          Teuchos::Copy))
    {}



    template <typename Number>
    Vector<Number>::Vector(const Teuchos::RCP<VectorType> V)
      : Subscriptor()
      , vector(V)
    {}



    template <typename Number>
    Vector<Number>::Vector(const IndexSet &parallel_partitioner,
                           const MPI_Comm  communicator)
      : Subscriptor()
      , vector(Utilities::Trilinos::internal::make_rcp<VectorType>(
          parallel_partitioner.make_tpetra_map_rcp(communicator, false)))
    {}



    template <typename Number>
    void
    Vector<Number>::reinit(const IndexSet &parallel_partitioner,
                           const MPI_Comm  communicator,
                           const bool      omit_zeroing_entries)
    {
      Teuchos::RCP<MapType> input_map =
        parallel_partitioner.make_tpetra_map_rcp(communicator, false);

      if (vector->getMap()->isSameAs(*input_map) == false)
        Utilities::Trilinos::internal::make_rcp<VectorType>(input_map);
      else if (omit_zeroing_entries == false)
        {
          vector->putScalar(0.);
        }
    }



    template <typename Number>
    void
    Vector<Number>::reinit(const IndexSet &locally_owned_entries,
                           const IndexSet &ghost_entries,
                           const MPI_Comm  communicator)
    {
      IndexSet parallel_partitioner = locally_owned_entries;
      parallel_partitioner.add_indices(ghost_entries);

      Teuchos::RCP<MapType> input_map =
        parallel_partitioner.make_tpetra_map_rcp(communicator, true);

      Utilities::Trilinos::internal::make_rcp<VectorType>(input_map);
    }



    template <typename Number>
    void
    Vector<Number>::reinit(const Vector<Number> &V,
                           const bool            omit_zeroing_entries)
    {
      reinit(V.locally_owned_elements(),
             V.get_mpi_communicator(),
             omit_zeroing_entries);
    }



    template <typename Number>
    void
    Vector<Number>::extract_subvector_to(
      const ArrayView<const types::global_dof_index> &indices,
      ArrayView<Number>                              &elements) const
    {
      AssertDimension(indices.size(), elements.size());
      const auto &vector = trilinos_vector();
      const auto &map    = vector.getMap();

#  if DEAL_II_TRILINOS_VERSION_GTE(13, 2, 0)
      auto vector_2d = vector.template getLocalView<Kokkos::HostSpace>(
        Tpetra::Access::ReadOnly);
#  else
      /*
       * For Trilinos older than 13.2 we would normally have to call
       * vector.template sync<Kokkos::HostSpace>() at this place in order
       * to sync between memory spaces. This is necessary for GPU support.
       * Unfortunately, we are in a const context here and cannot call to
       * sync() (which is a non-const member function).
       *
       * Let us choose to simply ignore this problem for such an old
       * Trilinos version.
       */
      auto vector_2d = vector.template getLocalView<Kokkos::HostSpace>();
#  endif
      auto vector_1d = Kokkos::subview(vector_2d, Kokkos::ALL(), 0);

      for (unsigned int i = 0; i < indices.size(); ++i)
        {
          AssertIndexRange(indices[i], size());
          const auto trilinos_i = map->getLocalElement(
            static_cast<TrilinosWrappers::types::int_type>(indices[i]));
          elements[i] = vector_1d(trilinos_i);
        }
    }



    template <typename Number>
    Vector<Number> &
    Vector<Number>::operator=(const Vector<Number> &V)
    {
      // Distinguish three cases:
      //  - First case: both vectors have the same layout.
      //  - Second case: both vectors have the same size but different layout.
      //  - Third case: the vectors have different size.
      if (vector->getMap()->isSameAs(*(V.trilinos_vector().getMap())))
        *vector = V.trilinos_vector();
      else
        {
          if (size() == V.size())
            {
              Tpetra::Import<int, types::signed_global_dof_index> data_exchange(
                vector->getMap(), V.trilinos_vector().getMap());

              vector->doImport(V.trilinos_vector(),
                               data_exchange,
                               Tpetra::REPLACE);
            }
          else
            vector = Utilities::Trilinos::internal::make_rcp<VectorType>(
              V.trilinos_vector());
        }

      return *this;
    }



    template <typename Number>
    Vector<Number> &
    Vector<Number>::operator=(const Number s)
    {
      Assert(s == Number(0.),
             ExcMessage("Only 0 can be assigned to a vector."));

      vector->putScalar(s);

      return *this;
    }



    template <typename Number>
    void
    Vector<Number>::import_elements(
      const ReadWriteVector<Number> &V,
      VectorOperation::values        operation,
      const Teuchos::RCP<const Utilities::MPI::CommunicationPatternBase>
        &communication_pattern)
    {
      // If no communication pattern is given, create one. Otherwise, use the
      // one given.
      if (communication_pattern.is_null())
        {
          // The first time import is called, a communication pattern is
          // created. Check if the communication pattern already exists and if
          // it can be reused.
          if ((source_stored_elements.size() !=
               V.get_stored_elements().size()) ||
              (source_stored_elements != V.get_stored_elements()))
            {
              const Teuchos::MpiComm<int> *mpi_comm =
                dynamic_cast<const Teuchos::MpiComm<int> *>(
                  vector->getMap()->getComm().get());
              Assert(mpi_comm != nullptr, ExcInternalError());
              create_tpetra_comm_pattern(V.get_stored_elements(),
                                         *(mpi_comm->getRawMpiComm())());
            }
        }
      else
        {
          tpetra_comm_pattern = Teuchos::rcp_dynamic_cast<
            const TpetraWrappers::CommunicationPattern>(communication_pattern);

          AssertThrow(
            !tpetra_comm_pattern.is_null(),
            ExcMessage(
              std::string("The communication pattern is not of type ") +
              "LinearAlgebra::TpetraWrappers::CommunicationPattern."));
        }

      Teuchos::RCP<const Tpetra::Export<int, types::signed_global_dof_index>>
        tpetra_export = tpetra_comm_pattern->get_tpetra_export_rcp();

      VectorType source_vector(tpetra_export->getSourceMap());

      {
#  if DEAL_II_TRILINOS_VERSION_GTE(13, 2, 0)
        auto x_2d = source_vector.template getLocalView<Kokkos::HostSpace>(
          Tpetra::Access::ReadWrite);
#  else
        source_vector.template sync<Kokkos::HostSpace>();
        auto x_2d = source_vector.template getLocalView<Kokkos::HostSpace>();
#  endif
        auto x_1d = Kokkos::subview(x_2d, Kokkos::ALL(), 0);
#  if !DEAL_II_TRILINOS_VERSION_GTE(13, 2, 0)
        source_vector.template modify<Kokkos::HostSpace>();
#  endif
        const size_t localLength = source_vector.getLocalLength();
        auto         values_it   = V.begin();
        for (size_t k = 0; k < localLength; ++k)
          x_1d(k) = *values_it++;
#  if !DEAL_II_TRILINOS_VERSION_GTE(13, 2, 0)
        source_vector.template sync<
          typename Tpetra::Vector<Number, int, types::signed_global_dof_index>::
            device_type::memory_space>();
#  endif
      }
      if (operation == VectorOperation::insert)
        vector->doExport(source_vector, *tpetra_export, Tpetra::REPLACE);
      else if (operation == VectorOperation::add)
        vector->doExport(source_vector, *tpetra_export, Tpetra::ADD);
      else
        AssertThrow(false, ExcNotImplemented());
    }



    template <typename Number>
    void
    Vector<Number>::import_elements(
      const ReadWriteVector<Number> &V,
      VectorOperation::values        operation,
      const std::shared_ptr<const Utilities::MPI::CommunicationPatternBase> &)
    {
      import_elements(V, operation);
    }


    template <typename Number>
    void
    Vector<Number>::import_elements(const ReadWriteVector<Number> &V,
                                    VectorOperation::values        operation)
    {
      // Create an empty CommunicationPattern
      const Teuchos::RCP<const Utilities::MPI::CommunicationPatternBase>
        communication_pattern_empty;

      import_elements(V, operation, communication_pattern_empty);
    }



    template <typename Number>
    Vector<Number> &
    Vector<Number>::operator*=(const Number factor)
    {
      AssertIsFinite(factor);
      vector->scale(factor);

      return *this;
    }



    template <typename Number>
    Vector<Number> &
    Vector<Number>::operator/=(const Number factor)
    {
      AssertIsFinite(factor);
      Assert(factor != Number(0.), ExcZero());
      *this *= Number(1.) / factor;

      return *this;
    }



    template <typename Number>
    Vector<Number> &
    Vector<Number>::operator+=(const Vector<Number> &V)
    {
      // If the maps are the same we can update right away.
      if (vector->getMap()->isSameAs(*(V.trilinos_vector().getMap())))
        {
          vector->update(1., V.trilinos_vector(), 1.);
        }
      else
        {
          Assert(this->size() == V.size(),
                 ExcDimensionMismatch(this->size(), V.size()));

          // TODO: Tpetra doesn't have a combine mode that also updates local
          // elements, maybe there is a better workaround.
          Tpetra::Vector<Number, int, types::signed_global_dof_index> dummy(
            vector->getMap(), false);
          Tpetra::Import<int, types::signed_global_dof_index> data_exchange(
            V.trilinos_vector().getMap(), dummy.getMap());

          dummy.doImport(V.trilinos_vector(), data_exchange, Tpetra::INSERT);

          vector->update(1.0, dummy, 1.0);
        }

      return *this;
    }



    template <typename Number>
    Vector<Number> &
    Vector<Number>::operator-=(const Vector<Number> &V)
    {
      this->add(-1., V);

      return *this;
    }



    template <typename Number>
    Number
    Vector<Number>::operator*(const Vector<Number> &V) const
    {
      Assert(this->size() == V.size(),
             ExcDimensionMismatch(this->size(), V.size()));
      Assert(vector->getMap()->isSameAs(*V.trilinos_vector().getMap()),
             ExcDifferentParallelPartitioning());

      return vector->dot(V.trilinos_vector());
    }



    template <typename Number>
    void
    Vector<Number>::add(const Number a)
    {
      AssertIsFinite(a);

#  if DEAL_II_TRILINOS_VERSION_GTE(13, 2, 0)
      auto vector_2d = vector->template getLocalView<Kokkos::HostSpace>(
        Tpetra::Access::ReadWrite);
#  else
      vector->template sync<Kokkos::HostSpace>();
      auto vector_2d = vector->template getLocalView<Kokkos::HostSpace>();
#  endif
      auto vector_1d = Kokkos::subview(vector_2d, Kokkos::ALL(), 0);
#  if !DEAL_II_TRILINOS_VERSION_GTE(13, 2, 0)
      vector->template modify<Kokkos::HostSpace>();
#  endif
      const size_t localLength = vector->getLocalLength();
      for (size_t k = 0; k < localLength; ++k)
        {
          vector_1d(k) += a;
        }
#  if !DEAL_II_TRILINOS_VERSION_GTE(13, 2, 0)
      vector->template sync<
        typename Tpetra::Vector<Number, int, types::signed_global_dof_index>::
          device_type::memory_space>();
#  endif
    }



    template <typename Number>
    void
    Vector<Number>::add(const Number a, const Vector<Number> &V)
    {
      AssertIsFinite(a);
      Assert(vector->getMap()->isSameAs(*(V.trilinos_vector().getMap())),
             ExcDifferentParallelPartitioning());

      vector->update(a, V.trilinos_vector(), 1.);
    }



    template <typename Number>
    void
    Vector<Number>::add(const Number          a,
                        const Vector<Number> &V,
                        const Number          b,
                        const Vector<Number> &W)
    {
      Assert(vector->getMap()->isSameAs(*(V.trilinos_vector().getMap())),
             ExcDifferentParallelPartitioning());
      Assert(vector->getMap()->isSameAs(*(W.trilinos_vector().getMap())),
             ExcDifferentParallelPartitioning());
      AssertIsFinite(a);
      AssertIsFinite(b);

      vector->update(a, V.trilinos_vector(), b, W.trilinos_vector(), 1.);
    }



    template <typename Number>
    void
    Vector<Number>::sadd(const Number          s,
                         const Number          a,
                         const Vector<Number> &V)
    {
      *this *= s;

      Vector<Number> tmp(V);
      tmp *= a;
      *this += tmp;
    }



    template <typename Number>
    void
    Vector<Number>::scale(const Vector<Number> &scaling_factors)
    {
      Assert(vector->getMap()->isSameAs(
               *(scaling_factors.trilinos_vector().getMap())),
             ExcDifferentParallelPartitioning());

      vector->elementWiseMultiply(1., *scaling_factors.vector, *vector, 0.);
    }



    template <typename Number>
    void
    Vector<Number>::equ(const Number a, const Vector<Number> &V)
    {
      // If we don't have the same map, copy.
      if (vector->getMap()->isSameAs(*V.trilinos_vector().getMap()) == false)
        this->sadd(0., a, V);
      else
        {
          // Otherwise, just update
          vector->update(a, V.trilinos_vector(), 0.);
        }
    }



    template <typename Number>
    bool
    Vector<Number>::all_zero() const
    {
      // get a representation of the vector and
      // loop over all the elements
      Number       *start_ptr = vector->getDataNonConst().get();
      const Number *ptr       = start_ptr,
                   *eptr      = start_ptr + vector->getLocalLength();
      unsigned int flag       = 0;
      while (ptr != eptr)
        {
          if (*ptr != Number(0))
            {
              flag = 1;
              break;
            }
          ++ptr;
        }

      // Check that the vector is zero on _all_ processors.
      const Teuchos::MpiComm<int> *mpi_comm =
        dynamic_cast<const Teuchos::MpiComm<int> *>(
          vector->getMap()->getComm().get());
      Assert(mpi_comm != nullptr, ExcInternalError());
      unsigned int num_nonzero =
        Utilities::MPI::sum(flag, *(mpi_comm->getRawMpiComm())());

      return num_nonzero == 0;
    }



    template <typename Number>
    Number
    Vector<Number>::mean_value() const
    {
      return vector->meanValue();
    }



    template <typename Number>
    typename Vector<Number>::real_type
    Vector<Number>::l1_norm() const
    {
      return vector->norm1();
    }



    template <typename Number>
    typename Vector<Number>::real_type
    Vector<Number>::l2_norm() const
    {
      return vector->norm2();
    }



    template <typename Number>
    typename Vector<Number>::real_type
    Vector<Number>::linfty_norm() const
    {
      return vector->normInf();
    }



    template <typename Number>
    Number
    Vector<Number>::add_and_dot(const Number          a,
                                const Vector<Number> &V,
                                const Vector<Number> &W)
    {
      this->add(a, V);

      return *this * W;
    }



    template <typename Number>
    typename Vector<Number>::size_type
    Vector<Number>::size() const
    {
      return vector->getGlobalLength();
    }



    template <typename Number>
    typename Vector<Number>::size_type
    Vector<Number>::locally_owned_size() const
    {
      return vector->getLocalLength();
    }



    template <typename Number>
    MPI_Comm
    Vector<Number>::get_mpi_communicator() const
    {
      const auto *const tpetra_comm =
        dynamic_cast<const Teuchos::MpiComm<int> *>(
          vector->getMap()->getComm().get());
      Assert(tpetra_comm != nullptr, ExcInternalError());
      return *(tpetra_comm->getRawMpiComm())();
    }



    template <typename Number>
    ::dealii::IndexSet
    Vector<Number>::locally_owned_elements() const
    {
      IndexSet is(size());

      // easy case: local range is contiguous
      if (vector->getMap()->isContiguous())
        {
          is.add_range(vector->getMap()->getMinGlobalIndex(),
                       vector->getMap()->getMaxGlobalIndex() + 1);
        }
      else if (vector->getLocalLength() > 0)
        {
          const size_type n_indices = vector->getLocalLength();
          std::vector<types::global_dof_index> vector_indices;
          vector_indices.reserve(n_indices);
          for (unsigned int i = 0; i < n_indices; ++i)
            vector_indices.push_back(vector->getMap()->getGlobalElement(i));

          is.add_indices(vector_indices.data(),
                         vector_indices.data() + n_indices);
        }
      is.compress();

      return is;
    }



    template <typename Number>
    void
    Vector<Number>::compress(const VectorOperation::values /*operation*/)
    {}


    template <typename Number>
    const Tpetra::Vector<Number, int, types::signed_global_dof_index> &
    Vector<Number>::trilinos_vector() const
    {
      return *vector;
    }



    template <typename Number>
    Tpetra::Vector<Number, int, types::signed_global_dof_index> &
    Vector<Number>::trilinos_vector()
    {
      return *vector;
    }



    template <typename Number>
    Teuchos::RCP<Tpetra::Vector<Number, int, types::signed_global_dof_index>>
    Vector<Number>::trilinos_rcp()
    {
      return vector;
    }



    template <typename Number>
    Teuchos::RCP<
      const Tpetra::Vector<Number, int, types::signed_global_dof_index>>
    Vector<Number>::trilinos_rcp() const
    {
      return vector.getConst();
    }



    template <typename Number>
    void
    Vector<Number>::print(std::ostream      &out,
                          const unsigned int precision,
                          const bool         scientific,
                          const bool         across) const
    {
      AssertThrow(out.fail() == false, ExcIO());
      boost::io::ios_flags_saver restore_flags(out);

      // Get a representation of the vector and loop over all
      // the elements
      const auto val = vector->get1dView();

      out.precision(precision);
      if (scientific)
        out.setf(std::ios::scientific, std::ios::floatfield);
      else
        out.setf(std::ios::fixed, std::ios::floatfield);

#  if DEAL_II_TRILINOS_VERSION_GTE(13, 2, 0)
      auto vector_2d = vector->template getLocalView<Kokkos::HostSpace>(
        Tpetra::Access::ReadOnly);
#  else
      vector->template sync<Kokkos::HostSpace>();
      auto vector_2d = vector->template getLocalView<Kokkos::HostSpace>();
#  endif
      auto         vector_1d    = Kokkos::subview(vector_2d, Kokkos::ALL(), 0);
      const size_t local_length = vector->getLocalLength();

      if (across)
        for (unsigned int i = 0; i < local_length; ++i)
          out << vector_1d(i) << ' ';
      else
        for (unsigned int i = 0; i < local_length; ++i)
          out << vector_1d(i) << std::endl;
      out << std::endl;

      // restore the representation
      // of the vector
      AssertThrow(out.fail() == false, ExcIO());
    }


    template <typename Number>
    MPI_Comm
    Vector<Number>::mpi_comm() const
    {
      MPI_Comm out;
#  ifdef DEAL_II_WITH_MPI
      const Teuchos::MpiComm<int> *mpi_comm =
        dynamic_cast<const Teuchos::MpiComm<int> *>(
          vector->getMap()->getComm().get());
      out = *mpi_comm->getRawMpiComm();
#  else
      out            = MPI_COMM_SELF;
#  endif
      return out;
    }


    template <typename Number>
    std::size_t
    Vector<Number>::memory_consumption() const
    {
      return sizeof(*this) +
             vector->getLocalLength() *
               (sizeof(Number) + sizeof(TrilinosWrappers::types::int_type));
    }



    template <typename Number>
    void
    Vector<Number>::create_tpetra_comm_pattern(const IndexSet &source_index_set,
                                               const MPI_Comm  mpi_comm)
    {
      source_stored_elements = source_index_set;
      tpetra_comm_pattern    = Utilities::Trilinos::internal::make_rcp<
        TpetraWrappers::CommunicationPattern>(locally_owned_elements(),
                                              source_index_set,
                                              mpi_comm);
    }
  } // namespace TpetraWrappers
} // namespace LinearAlgebra

DEAL_II_NAMESPACE_CLOSE

#endif

#endif
