//
// Canadian Hydrological Model - The Canadian Hydrological Model (CHM) is a novel
// modular unstructured mesh based approach for hydrological modelling
// Copyright (C) 2018 Christopher Marsh
//
// This file is part of Canadian Hydrological Model.
//
// Canadian Hydrological Model is free software: you can redistribute it and/or
// modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Canadian Hydrological Model is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Canadian Hydrological Model.  If not, see
// <http://www.gnu.org/licenses/>.
//


#include "H5Cpp.h"
#include "logger.hpp"
#include "triangulation.hpp"
#include "sort_perm.hpp"
#include "core.hpp"

#include <string>
#include <boost/property_tree/ptree.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>

namespace pt = boost::property_tree;
namespace po = boost::program_options;

using namespace H5;

typedef struct _MeshParameters
{
    std::vector<std::string> names;
} MeshParameters;




// this is declared in the main triangulation.cpp
herr_t group_info(hid_t loc_id, const char *name, const H5L_info_t *linfo, void *opdata);

class preprocessingTriangulation : public triangulation
{
  public:

    preprocessingTriangulation()
    {
        _is_standalone = false;
        _is_partition = true;
    }

    void partition(int mpi_rank)
    { // Set up so that all processors know how 'big' all other processors are
        _num_faces_in_partition.resize(_comm_world.size(), _num_global_faces / _comm_world.size());
        for (unsigned int i = 0; i < _num_global_faces % _comm_world.size(); ++i)
        {
            _num_faces_in_partition.at(i)++;
        }

        // each processor only knows its own start and end indices
        size_t face_start_idx = 0;
        size_t face_end_idx = _num_faces_in_partition.at(0) - 1;
        for (int i = 1; i <= _comm_world.rank(); ++i)
        {
            face_start_idx += _num_faces_in_partition.at(i - 1);
            face_end_idx += _num_faces_in_partition.at(i);
        }
        // Store in the public members
        global_cell_start_idx = face_start_idx;
        global_cell_end_idx = face_end_idx;

        // Set size of vector containing locally owned faces
        _local_faces.resize(_num_faces_in_partition.at(_comm_world.rank()));

        // Loop can't be parallel due to modifying map
        for (size_t local_ind = 0; local_ind < _local_faces.size(); ++local_ind)
        {
            size_t offset_idx = face_start_idx + local_ind;
            _global_to_locally_owned_index_map[_global_IDs.at(offset_idx)] = local_ind;

            _faces.at(_global_IDs.at(offset_idx))->is_ghost = false;
            _faces.at(_global_IDs.at(offset_idx))->owner = mpi_rank;
            _faces.at(_global_IDs.at(offset_idx))->cell_local_id = local_ind;
            _local_faces.at(local_ind) = _faces.at(_global_IDs.at(offset_idx));

        }

        LOG_DEBUG << "MPI Process " << mpi_rank << ": start " << face_start_idx << ", end " << face_end_idx
                  << ", number " << _local_faces.size();
    }
    void read_h5(const std::string& mesh_filename)
    {
        std::vector<Point_2> center_points;

        try
        {
            // Turn off the auto-printing when failure occurs so that we can
            // handle the errors appropriately
            Exception::dontPrint();

            // Open an existing file and dataset.
            H5File file(mesh_filename, H5F_ACC_RDONLY);

            std::vector<std::array<double, 3>> vertex;
            std::vector<std::array<int, 3>> elem;
            std::vector<std::array<int, 3>> neigh;

            {
                // Read the proj4
                H5::DataSpace dataspace(1, &proj4_dims);
                H5::Attribute attribute = file.openAttribute("/mesh/proj4");
                attribute.read(proj4_t, _srs_wkt);

            }

            {  // Write the is_geographic
                H5::DataSpace dataspace(1, &geographic_dims);
                H5::Attribute attribute = file.openAttribute("/mesh/is_geographic");
                attribute.read(PredType::NATIVE_HBOOL, &_is_geographic);

            }

            {
                DataSet dataset = file.openDataSet("/mesh/cell_global_id");
                DataSpace dataspace = dataset.getSpace();
                hsize_t nelem;
                int ndims = dataspace.getSimpleExtentDims(&nelem, NULL);
                _global_IDs.resize(nelem);
                dataset.read(_global_IDs.data(), PredType::NATIVE_INT);
            }

            {
                // Open the vertices dataset
                DataSet dataset = file.openDataSet("/mesh/vertex");
                DataSpace dataspace = dataset.getSpace();

                hsize_t nvert;
                int ndims = dataspace.getSimpleExtentDims(&nvert, NULL);

                // Ensure enough space in the vector
                vertex.resize(nvert);
                // Default args read all of the dataspace
                dataset.read(vertex.data(), vertex_t);

                for (size_t i = 0; i < nvert; i++)
                {

                    Point_3 pt(vertex[i][0], vertex[i][1], vertex[i][2]);
                    _max_z = std::max(_max_z, vertex[i][2]);
                    _min_z = std::min(_min_z, vertex[i][2]);

                    Vertex_handle Vh = create_vertex();
                    Vh->set_point(pt);
                    Vh->set_id(i);
                    _vertexes.push_back(Vh);
                }

                _num_vertex = _vertexes.size();
                LOG_DEBUG << "# nodes created = " << _num_vertex;
            }

            {
                DataSet dataset = file.openDataSet("/mesh/elem");
                DataSpace dataspace = dataset.getSpace();

                hsize_t nelem;
                int ndims = dataspace.getSimpleExtentDims(&nelem, NULL);

                // Ensure enough space in the vector
                elem.resize(nelem);
                // Default args read all of the dataspace
                dataset.read(elem.data(), elem_t);

                for (size_t i = 0; i < nelem; i++)
                {

                    auto vert1 = _vertexes.at(elem[i][0]); // 0 indexing
                    auto vert2 = _vertexes.at(elem[i][1]);
                    auto vert3 = _vertexes.at(elem[i][2]);

                    auto face = create_face(vert1, vert2, vert3);
                    face->cell_global_id = i;
                    face->cell_local_id = i;

                    if (_is_geographic)
                    {
                        face->_is_geographic = true;
                    }

                    face->_debug_ID = -(i + 1); // all ids will be negative starting at -1. Named ids (for output) will
                                                // be positive starting at 0
                    face->_debug_name = std::to_string(i);


                    vert1->set_face(face);
                    vert2->set_face(face);
                    vert3->set_face(face);

                    _faces.push_back(face);
                }

                _num_faces = _faces.size();
                LOG_DEBUG << "Created a mesh with " << size_faces() << " triangles";
            }

            {

                DataSet dataset = file.openDataSet("/mesh/neighbor");
                DataSpace dataspace = dataset.getSpace();

                hsize_t nelem;
                int ndims = dataspace.getSimpleExtentDims(&nelem, NULL);

                if (elem.size() != nelem)
                {
                    BOOST_THROW_EXCEPTION(config_error()
                                          << errstr_info("Expected: " + std::to_string(elem.size()) +
                                                         " neighborlists, got: " + std::to_string(nelem)));
                }

                // Ensure enough space in the vector
                neigh.resize(nelem);
                // Default args read all of the dataspace
                dataset.read(neigh.data(), neighbor_t);

                LOG_DEBUG << "Building face neighbors";
                for (size_t i = 0; i < nelem; i++)
                {
                    auto face = _faces.at(i);

                    if (neigh[i][0] > static_cast<int>(nelem) || neigh[i][1] > static_cast<int>(nelem) ||
                        neigh[i][2] > static_cast<int>(nelem))
                    {
                        BOOST_THROW_EXCEPTION(config_error() << errstr_info("Face " + std::to_string(i) +
                                                                            " has out of bound neighbors."));
                    }

                    //-1 is now the no neighbor value
                    Face_handle face0 = neigh[i][0] != -1 ? _faces.at(neigh[i][0]) : nullptr; // Face_handle()
                    Face_handle face1 = neigh[i][1] != -1 ? _faces.at(neigh[i][1]) : nullptr;
                    Face_handle face2 = neigh[i][2] != -1 ? _faces.at(neigh[i][2]) : nullptr;

                    face->set_neighbors(face0, face1, face2);
                }
            }

        } // end of try block

        // catch failure caused by the H5File operations
        catch (FileIException& error)
        {
            error.printErrorStack();
        }

        // catch failure caused by the DataSet operations
        catch (DataSetIException& error)
        {
            error.printErrorStack();
        }
    }
    void determine_local_boundary_faces()
    {
        /*
          - Store handles to boundary faces on the locally owned process
          - Also store boolean value "is_global_boundary"
        */

        using th_safe_multicontainer_type = std::vector< std::pair<mesh_elem,bool> >[];


        // Need to ensure we're starting from nothing?
        assert( _boundary_faces.size() == 0 );

        LOG_DEBUG << "Determining local boundary faces";

        std::unique_ptr< th_safe_multicontainer_type > th_local_boundary_faces;

#pragma omp parallel
        {
            // We want an array of vectors, so that OMP threads can increment them
            // separately, then join them afterwards
#pragma omp single
            {
                th_local_boundary_faces = std::make_unique< th_safe_multicontainer_type >(omp_get_num_threads());
            }
#pragma omp for
            for(size_t face_index=0; face_index< _local_faces.size(); ++face_index)
            {
                // face_index is a local index... get the face handle
                auto face = _local_faces.at(face_index);

                int num_owned_neighbors = 0;
                for (int neigh_index = 0; neigh_index < 3; ++neigh_index)
                {

                    auto neigh = face->neighbor(neigh_index);

                    // Test status of neighbor
                    if (neigh == nullptr)
                    {
                        th_local_boundary_faces[omp_get_thread_num()].push_back(std::make_pair(face,true));
                        num_owned_neighbors=3; // set this to avoid triggering the post-loop if statement
                        break;
                    } else
                    {
                        if (neigh->is_ghost == false)
                        {
                            num_owned_neighbors++;
                        }
                    }
                }

                // If we don't own 3 neighbors, we are a local, but not a global boundary face
                if( num_owned_neighbors<3 ) {
                    th_local_boundary_faces[omp_get_thread_num()].push_back(std::make_pair(face,false));
                }
            }

            // Join the vectors via a single thread in t operations
            //  NOTE future optimizations:
            //   - reserve space for insertions into _boundary_faces
            //   - can be done recursively in log2(t) operations
#pragma omp single
            {
                for(int thread_idx=0;thread_idx<omp_get_num_threads();++thread_idx) {
                    _boundary_faces.insert(_boundary_faces.end(),
                                           th_local_boundary_faces[thread_idx].begin(), th_local_boundary_faces[thread_idx].end());
                }
            }
        }

        // Some log debug output to see how many boundary faces on each
        LOG_DEBUG << "MPI Process " << _comm_world.rank() << " has " << _boundary_faces.size() << " boundary faces.";

        LOG_DEBUG << "MPI Process " << _comm_world.rank() << " _faces.size(): " << _faces.size() << " _local_faces.size(): " << _local_faces.size();


    }
    void determine_process_ghost_faces_nearest_neighbors()
    {
        // NOTE that this algorithm is not implemented for multithread
        // - multithread can be implemented similarly to determine_local_boundary_faces
        //   - not a priority since number of boundary faces should be small

        // Ensure that the local boundary faces have been determined, but ghost nearest neighbors have not been set
        assert( _boundary_faces.size() != 0 );
        assert( _ghost_neighbors.size() == 0 );

        LOG_DEBUG << "Determining ghost region info";

        // Vector for append speed
        std::vector< mesh_elem > ghosted_boundary_nearest_neighbors;

        for(size_t face_index=0; face_index< _boundary_faces.size(); ++face_index)
        {
            // face_index is a local index... get the face handle
            auto face = _boundary_faces.at(face_index).first;
            // append the ghosted nearest neighbors
            for(int i = 0; i < 3; ++i)
            {
                auto neigh = face->neighbor(i);
                if(neigh != nullptr && neigh->is_ghost)
                {
                    ghosted_boundary_nearest_neighbors.push_back(neigh);
                    neigh->get_module_data<ghost_info>("partition_tool")->ghost_type = ghost_info::GHOST_TYPE::NEIGH;
                }
            }
        }

        // Convert to a set to remove duplicates
        std::unordered_set<mesh_elem> tmp_set(std::begin(ghosted_boundary_nearest_neighbors),
                                              std::end(ghosted_boundary_nearest_neighbors));
        // Convert the set to a vector
        _ghost_neighbors.insert(std::end(_ghost_neighbors),
                                std::begin(tmp_set),std::end(tmp_set));
        // Sort the ghost neighbors
        // NOTE:
        // - sorting this vector by cell_global_id effectively partitions the ghost neighbors to be contiguous in communication partners
        std::sort(_ghost_neighbors.begin(),_ghost_neighbors.end(),
                  [&](const auto& a, const auto& b)
                  {
                    return a->cell_global_id < b->cell_global_id;
                  });


        LOG_DEBUG << "MPI Process " << _comm_world.rank() << " has " << _ghost_neighbors.size() << " ghosted nearest neighbors.";


        // Determine the owners of the ghost faces (for communication setup)
        _ghost_neighbor_owners.resize(_ghost_neighbors.size());
        int start_index=0;
        int prev_owner;
        int num_partners=0;
        // Construct ghost region ownership info
        for(size_t i=0; i<_ghost_neighbors.size(); ++i)
        {
            // index type needs to match type of elements of _num_faces_in_partition
            int global_ind = static_cast<int>(_ghost_neighbors[i]->cell_global_id);
            _ghost_neighbor_owners[i] = determine_owner_of_global_index(global_ind,
                                                                        _num_faces_in_partition);
            // on first it, no value of prev_owner exists
            if(i==0) prev_owner = _ghost_neighbor_owners[i];
            // if owner different from last iteration, store prev segment's ownership info
            if (prev_owner != _ghost_neighbor_owners[i])
            {
                num_partners++;
                _comm_partner_ownership[prev_owner] = std::make_pair(start_index, i-start_index);
                start_index=i;
            }

            if (i ==_ghost_neighbors.size()-1) {
                _comm_partner_ownership[prev_owner] = std::make_pair(start_index, i-start_index+1);
            }

            // prep prev_owner for next iteration
            prev_owner=_ghost_neighbor_owners[i];
        }

        for(size_t i=0; i<_ghost_neighbors.size(); ++i)
        {
            _global_index_to_local_ghost_map[_ghost_neighbors[i]->cell_global_id] = static_cast<int>(i);
        }


        LOG_DEBUG << "MPI Process " << _comm_world.rank() << " has " << _comm_partner_ownership.size() << " communication partners:";
        for (auto it : _comm_partner_ownership)
        {
            LOG_DEBUG << "  # Process " << _comm_world.rank() << " partner " << it.first << " owns local ghost indices " << it.second.first << " to " << it.second.first+it.second.second-1;
        }


    }


    void from_hdf5_and_partition(const std::string& mesh_filename,
                                 const std::vector<std::string>& param_filenames,
                                 size_t MPI_ranks,
                                 size_t max_ghost_distance,
                                 int standalone_rank)
    {


        _comm_world._size = MPI_ranks;

        {
            core c; // instantiate this long enough to use this one function
            _is_geographic = c.check_is_geographic(mesh_filename);
        }

        if( _is_geographic)
        {
            math::gis::point_from_bearing = & math::gis::point_from_bearing_latlong;
            math::gis::distance = &math::gis::distance_latlong;
        }
        else
        {
            math::gis::point_from_bearing = &math::gis::point_from_bearing_UTM;
            math::gis::distance = &math::gis::distance_UTM;
        }


        auto mesh_file_extension = boost::filesystem::path(mesh_filename).extension().string();

        if(mesh_file_extension == ".partition")
        {
            CHM_THROW_EXCEPTION(mesh_error, "Cannot run this tool with a .partition mesh input!");
        }

        if(mesh_file_extension == ".h5")
        {
            LOG_DEBUG << "Partitioning mesh " << mesh_filename << " with #ranks=" << MPI_ranks;
            read_h5(mesh_filename);

        }
        else
        {
            auto mesh = read_json(mesh_filename);

            //mesh will have been loaded by the geographic check so don't re load it here
            bool triarea_found = false;

            // Parameter files
            for (auto param_file : param_filenames)
            {
                pt::ptree param_json = read_json(param_file);

                for(auto& ktr : param_json)
                {
                    //use put to ensure there are no duplciate parameters...
                    std::string key = ktr.first.data();
                    mesh.put_child( "parameters." + key ,ktr.second);

                    if( key == "area")
                        triarea_found = true;

                    LOG_DEBUG << "Inserted parameter " << ktr.first.data() << " into the config tree.";
                }
            }

            if(_is_geographic && !triarea_found)
            {
                BOOST_THROW_EXCEPTION(mesh_error() << errstr_info("Geographic meshes require the triangle area be present in a .param file. Please include this."));
            }

            // Initial condition files
//            for(auto ic_file : initial_condition_file_paths)
//            {
//                pt::ptree ic_json = read_json(ic_file);
//
//                for(auto& ktr : ic_json)
//                {
//                    //use put to ensure there are no duplciate parameters...
//                    std::string key = ktr.first.data();
//                    mesh.put_child( "initial_conditions." + key ,ktr.second);
//                    LOG_DEBUG << "Inserted initial condition " << ktr.first.data() << " into the config tree.";
//                }
//            }

            from_json(mesh);

            LOG_DEBUG << "Converting mesh to hdf5...";
            auto base_name = boost::filesystem::path(mesh_filename).stem().string();
            triangulation::to_hdf5(base_name);
            LOG_DEBUG << "HDF5 written, terminating";
            return;

        }


        _num_global_faces = _faces.size();

        pt::ptree tree;
        tree.put("ranks",MPI_ranks);
        tree.put("max_ghost_distance",max_ghost_distance);
        tree.put("num_global_faces",_num_global_faces);

        pt::ptree meshes;
        pt::ptree params;

        std::string filename_base = mesh_filename.substr(0,mesh_filename.length()-3);

        auto partition_dir = boost::filesystem::path(filename_base + ".np" +  std::to_string(_comm_world.size()) + ".partition.meshes");
        boost::filesystem::create_directory(partition_dir);

        int start_rank = 0;
        int end_rank = MPI_ranks;
        if(standalone_rank > -1 )
        {
            start_rank = standalone_rank;
            end_rank = standalone_rank+1;
            _is_standalone = true;
        }
        for (int mpirank = start_rank; mpirank < end_rank; mpirank++)
        {
            _comm_world._rank = mpirank;

            // reset everything
            _global_to_locally_owned_index_map.clear();
            _num_faces_in_partition = {};
            _local_faces = {};
            _boundary_faces = {};
            _ghost_neighbors = {};
            _ghost_neighbor_owners = {};
            _ghost_faces = {};
            _comm_partner_ownership.clear();
            _global_index_to_local_ghost_map.clear();
            global_indices_to_send.clear();
            _param_data.clear();


            std::set<std::string> pt = {"partition_tool"};

            #pragma omp parallel for
            for(size_t i = 0; i < _faces.size(); ++i)
            {
                _faces.at(i)->is_ghost = true; // default state, switches to false later
                _faces.at(i)->init_module_data(pt);

                auto *gi = _faces.at(i)->make_module_data<ghost_info>("partition_tool");

                // doesn't match the is_ghost default state. Here we assume false, and then switch it to the correct type when determined
                gi->ghost_type = ghost_info::GHOST_TYPE::NONE;
            }

            partition(_comm_world._rank);

            _num_faces = _local_faces.size();

            determine_local_boundary_faces();
            determine_process_ghost_faces_nearest_neighbors();

            // TODO: Need to auto-determine how far to look based on module setups
            determine_process_ghost_faces_by_distance(100.0);


            // we need to flag all the ghosts that aren't neigh as radius ghosts
            #pragma omp parallel for
            for(size_t i = 0; i < _ghost_faces.size(); i++)
            {
                auto face = _ghost_faces.at(i);
                auto* gi = face->get_module_data<ghost_info>("partition_tool");

                // if we aren't a neigh, we were added by determine_process_ghost_faces_by_distance
                if(gi->ghost_type != ghost_info::GHOST_TYPE::NEIGH)
                    gi->ghost_type = ghost_info::GHOST_TYPE::DIST;
            }

            // load params

            // Space for extracting the parameter info
            std::unique_ptr<MeshParameters> pars(new MeshParameters);
            for (auto param_filename : param_filenames)
            {
                try
                {
                    // Turn off the auto-printing when failure occurs so that we can
                    // handle the errors appropriately
                    Exception::dontPrint();

                    // Open an existing file and dataset.
                    H5File file(param_filename, H5F_ACC_RDONLY);

                    // Extract all of the parameter info (names) from the file
                    Group group = file.openGroup("parameters");
                    herr_t idx =
                        H5Literate(group.getId(), H5_INDEX_NAME, H5_ITER_INC, NULL, group_info, (void*)pars.get());

                    for (auto const& name : pars->names)
                    {
                        _parameters.insert(name);
                    }

                    for (auto const& name : pars->names)
                    {
                        // hyperslab size that is as large as just the local meshes to read the block
                        hsize_t local_size = static_cast<hsize_t>(_local_faces.size());

                        if (!_is_standalone)
                        {
                            // but we need to ensure we have enough room for local + ghosts
                            _param_data[name].resize(_local_faces.size() + _ghost_faces.size());
                        }
                        else
                        {
                            _param_data[name].resize(_local_faces.size());
                        }
                        hsize_t offset = static_cast<hsize_t>(_local_faces[0]->cell_global_id);

                        DataSet dataset = group.openDataSet(name);
                        DataSpace dataspace = dataset.getSpace();

                        hsize_t nelem;
                        int ndims = dataspace.getSimpleExtentDims(&nelem);

                        dataspace.selectHyperslab(H5S_SELECT_SET, &local_size, &offset);

                        DataSpace memspace(1, &nelem);
                        hsize_t zero = 0;
                        memspace.selectHyperslab(H5S_SELECT_SET, &local_size, &zero);

                        dataset.read(_param_data[name].data(), PredType::NATIVE_DOUBLE, memspace, dataspace);

                        if (!_is_standalone)
                        {
                            // Read parameters for each ghost face individually
                            hsize_t one = 1;
                            // Do NOT do this loop in parallel (internal state of HDF5)
                            for (size_t i = 0; i < _ghost_faces.size(); i++)
                            {
                                auto face = _ghost_faces.at(i);
                                hsize_t global_id = face->cell_global_id;

                                // Position and size in file
                                dataspace.selectHyperslab(H5S_SELECT_SET, &one, &global_id);
                                // Position and size in variable
                                memspace.selectHyperslab(H5S_SELECT_SET, &one, &zero);

                                double value;
                                dataset.read(&value, PredType::NATIVE_DOUBLE, memspace, dataspace);

                                _param_data[name][_local_faces.size() + i] = value;
                            }
                        }
                    }
                }
                // catch failure caused by the H5File operations
                catch (FileIException& error)
                {
                    error.printErrorStack();
                }
                // catch failure caused by the DataSet operations
                catch (DataSetIException& error)
                {
                    error.printErrorStack();
                }
            }


            if(!_is_standalone)
            {
                // now, augment the local faces with our ghosts, so that the ghosts are
                // a) flagged and b) available in this subset
                // _ghost_faces includes /all/ the ghost faces: neighbour + distance
                _local_faces.insert(_local_faces.end(), _ghost_faces.begin(), _ghost_faces.end());
            }

            //ensure the id order is monotonic increasing
            auto perm = sort_permutation(_local_faces,
                                         [](mesh_elem const& fa, mesh_elem const& fb)
                                         {
                                           return fa->cell_global_id < fb->cell_global_id;
                                         });

            apply_permutation_in_place(_local_faces, perm);

            for (auto const& name : pars->names)
            {
                apply_permutation_in_place(_param_data[name], perm);
            }

            auto fname = filename_base + ".partition." + (_is_standalone ? "standalone." : "") + std::to_string(mpirank);

            to_hdf5(partition_dir, fname);

            pt::ptree m;
            m.put("",(partition_dir / (fname+ "_mesh.h5")).string());
            meshes.push_back(std::make_pair("",m));

            pt::ptree p;
            p.put("",(partition_dir / (fname+ "_param.h5")).string());
            params.push_back(std::make_pair("",p));
        }
        tree.add_child("meshes",meshes);
        tree.add_child("parameters",params);


        pt::write_json(filename_base + ".np" + std::to_string(_comm_world.size()) + ".partition", tree);

    }

    void to_hdf5(boost::filesystem::path partition_dir, std::string filename_base)
    {

        std::string filename = filename_base + "_mesh.h5";

        //  a global to local that works with the local_faces that has all the ghosts mixed in
        std::map<int,int> global_to_local_faces_index_map;

        try
        {
            // Turn off the auto-printing when failure occurs so that we can
            // handle the errors appropriately
            Exception::dontPrint();

            H5::H5File file( (partition_dir / filename).string(), H5F_ACC_TRUNC);
            H5::Group group(file.createGroup("/mesh"));

            // local_faces here is slightly different than anywhere else by, at this  point, it will have ghosts
            // mixed in with it. This is thus all the faces this partition knows about
            hsize_t ntri = _local_faces.size();

            { // global elem id
                LOG_DEBUG << "Writting Global IDs";
                H5::DataSpace dataspace(1, &ntri);
                auto globalIDs = std::vector<int>(ntri);

//                #pragma omp parallel for
                for (size_t i = 0; i < ntri; ++i)
                {
                    globalIDs.at(i) = _local_faces.at(i)->cell_global_id;
                    global_to_local_faces_index_map[globalIDs.at(i)] = i;
                }
                H5::DataSet dataset = file.createDataSet("/mesh/cell_global_id", PredType::STD_I32BE, dataspace);
                dataset.write(globalIDs.data(), PredType::NATIVE_INT);
            }

            std::set<size_t> vertex_global_id;
            // figure out all of the unique vertexes this subset needs and save their global id
            for (size_t i = 0; i < ntri; ++i)
            {
                auto f = _local_faces.at(i);
                for (size_t j = 0; j < 3; ++j)
                {
                    vertex_global_id.insert(f->vertex(j)->get_id()); // set of the vertexes we will need
                }
            }

            LOG_DEBUG << "This mesh partition has " << vertex_global_id.size() << " unique vertexes";
            // build a list of all the above identified vertexes and set their local id
            std::vector< Delaunay::Vertex_handle > local_vertexes; // vertexes that this part of the mesh needs
            local_vertexes.resize(vertex_global_id.size());
            size_t i = 0;
            for( const auto& itr: vertex_global_id)
            {
                auto vh = vertex( itr );
                vh->set_local_id(i);
                local_vertexes.at(i) = vh;
                ++i;
            }

            { // Which vertices define each face
                H5::DataSpace dataspace(1, &ntri);
                std::vector<std::array<int, 3>> elem(ntri);

                #pragma omp parallel for
                for (size_t i = 0; i < ntri; ++i)
                {
                    auto f = _local_faces.at(i);
                    for (size_t j = 0; j < 3; ++j)
                    {
                        elem[i][j] = f->vertex(j)->get_local_id(); //uses a per partition local id
                    }
                }

                H5::DataSet dataset = file.createDataSet("/mesh/elem", elem_t, dataspace);
                dataset.write(elem.data(), elem_t);
            }

            hsize_t nvert = local_vertexes.size();
            { // Vertices
                H5::DataSpace dataspace(1, &nvert);
                std::vector<std::array<double, 3>> vertices(nvert);

                #pragma omp parallel for
                for (size_t i = 0; i < nvert; ++i)
                {
                    auto v = local_vertexes.at(i);
                    vertices[i][0] = v->point().x();
                    vertices[i][1] = v->point().y();
                    vertices[i][2] = v->point().z();
                }

                H5::DataSet dataset = file.createDataSet("/mesh/vertex", vertex_t, dataspace);
                dataset.write(vertices.data(), vertex_t);
            }

            { // neighbours
                H5::DataSpace dataspace(1, &ntri);
                std::vector<std::array<int, 3>> neighbor(ntri);

                #pragma omp parallel for
                for (size_t i = 0; i < ntri; ++i)
                {
                    auto f = this->face(i);
                    for (size_t j = 0; j < 3; ++j)
                    {
                        auto neigh = f->neighbor(j);
                        if (neigh != nullptr && //we have a neighbour
                            global_to_local_faces_index_map.find(neigh->cell_global_id) != global_to_local_faces_index_map.end() ) // and we've seen this neighbour's global ID
                        {
                            neighbor.at(i)[j] = neigh->cell_global_id;
                        }
                        else
                        {
                            // they may have a triangle neighbour, but we haven't seen it, so  it is outside our ghost region
                            neighbor.at(i)[j] = -1;
                        }
                    }
                }
                H5::DataSet dataset = file.createDataSet("/mesh/neighbor", neighbor_t, dataspace);
                dataset.write(neighbor.data(), neighbor_t);
            }

            { // ghosts
                H5::DataSpace dataspace(1, &ntri);
                std::vector<int> is_ghost(ntri);
                #pragma omp parallel for
                for (size_t i = 0; i < ntri; ++i)
                {
                    // we need to save the following status for ghost faces:
                    // 0 = Not a ghost
                    // 1 = Neighbour ghost
                    // 2 = Radius search ghost
                    auto gi = _local_faces[i]->get_module_data<ghost_info>("partition_tool");
                    is_ghost[i] = gi->ghost_type;
                }
                H5::DataSet dataset = file.createDataSet("/mesh/ghost_type", PredType::STD_I32BE, dataspace);
                dataset.write(is_ghost.data(), PredType::NATIVE_INT);

            }

            // Ensure the proj4 string can fit in the HDF5 data type
            if (_srs_wkt.length() >= 256)
            {
                BOOST_THROW_EXCEPTION(config_error() << errstr_info("Proj4 string needs to be < 256. Length: " +
                                                                    std::to_string(_srs_wkt.length())));
            }
            { // Write the proj4
                H5::DataSpace dataspace(1, &proj4_dims);
                H5::Attribute attribute = file.createAttribute("/mesh/proj4", proj4_t, dataspace);
                attribute.write(proj4_t, _srs_wkt);
            }

            { // Write the is_geographic
                H5::DataSpace dataspace(1, &geographic_dims);
                H5::Attribute attribute =
                    file.createAttribute("/mesh/is_geographic", PredType::NATIVE_HBOOL, dataspace);
                attribute.write(PredType::NATIVE_HBOOL, &_is_geographic);
            }

            { // Write that this is part of a partitioned mesh
                _is_partition = true;
                if(_is_standalone)
                    _is_partition = false;

                H5::DataSpace dataspace(1, &partition_dims);
                H5::Attribute attribute =
                    file.createAttribute("/mesh/is_partition", PredType::NATIVE_HBOOL, dataspace);
                attribute.write(PredType::NATIVE_HBOOL, &_is_partition);
            }


        } // end of try block

        // catch failure caused by the H5File operations
        catch (FileIException& error)
        {
            error.printErrorStack();
        }

        // catch failure caused by the DataSet operations
        catch (DataSetIException& error)
        {
            error.printErrorStack();
        }

        ////////////////////////////////////////////////////////////////////////////
        ////////////////////////////////////////////////////////////////////////////
        // Parameters
        ////////////////////////////////////////////////////////////////////////////
        ////////////////////////////////////////////////////////////////////////////

        std::string par_filename = filename_base + "_param.h5";

        try
        {
            // Turn off the auto-printing when failure occurs so that we can
            // handle the errors appropriately
            Exception::dontPrint();

            H5::H5File file( (partition_dir / par_filename).string(), H5F_ACC_TRUNC);
            H5::Group group(file.createGroup("/parameters"));

            hsize_t ntri = _local_faces.size();

            for (auto& par_iter : _param_data)
            {
                const auto& p = par_iter.first;

                H5::DataSpace dataspace(1, &ntri);
                std::string par_location = "/parameters/" + p;
                H5::DataSet dataset = file.createDataSet(par_location, PredType::NATIVE_DOUBLE, dataspace);

                dataset.write(_param_data[p].data(), PredType::NATIVE_DOUBLE);
            }

        } // end try block
        // catch failure caused by the H5File operations
        catch (FileIException& error)
        {
            error.printErrorStack();
        }
        // catch failure caused by the DataSet operations
        catch (DataSetIException& error)
        {
            error.printErrorStack();
        }

    }

    class comm_world
    {
      public:

        int rank()
        {
            return _rank;
        }

        int size()
        {
            return _size;
        }

        int _size;
        int _rank;
    };

    comm_world _comm_world;

    std::map<std::string, std::vector<double> > _param_data;

    bool _is_partition; // the flag to write to the hdf5 file

    bool _is_standalone; // are we in standalone mode


    struct ghost_info : public face_info
    {
        // Note the type of ghost we are:
        // None = not a ghost
        // Neigh = Neighbour ghost
        // Dist = Distance search ghost
        enum GHOST_TYPE {NONE, NEIGH, DIST};
        GHOST_TYPE ghost_type = GHOST_TYPE::NONE;
    };
};

int main(int argc, char *argv[])
{
    BOOST_LOG_FUNCTION();

    std::string mesh_filename;
    size_t MPI_ranks = 2;
    size_t max_ghost_distance = 100;

    int standalone_rank = -1;

    po::options_description desc("Allowed options.");
    desc.add_options()
        ("help", "This message")
        ("mesh-file,m", po::value<std::string>(&mesh_filename), "Mesh file")
        ("param-file,p", po::value<std::vector<std::string>>(), "Parameter file")
        ("max-ghost-distance,g", po::value<size_t>(&max_ghost_distance), "Parameter file")
        ("standalone,s", po::value<int>(&standalone_rank), "Write the paritioned mesh so-as to be loadable standalone. Advanced debugging feature. Arg is rank to do")
        ("mpi-ranks",po::value<size_t>(&MPI_ranks), "Number of MPI ranks to partition for");

    po::variables_map vm;
    po::store(po::command_line_parser(argc, argv).options(desc).run(), vm);
    po::notify(vm);


    auto is_json_mesh = boost::filesystem::path(mesh_filename).extension().string() == ".mesh";

    if(is_json_mesh)
    {
        LOG_WARNING << "The input mesh is in json format. It MUST be converted to hdf5 before it can be partitioned.\n"
                       " Once the hdf5 conversion is done, this tool will re-run with the h5 mesh as input to fully partition the mesh.";
    }



    if (!vm.count("mesh-file"))
    {
        LOG_ERROR << "Mesh file required";
        exit(-1);
    }
    if (!vm.count("param-file"))
    {
        LOG_ERROR << "Param file required";
        exit(-1);
    }
    if (!vm.count("mpi-ranks") && !is_json_mesh)
    {
        LOG_ERROR << "MPI ranks required";
        exit(-1);
    }

    if (vm.count("mpi-ranks") && is_json_mesh)
    {
        LOG_WARNING << "MPI ranks will be ignored for the json -> h5 conversion. ";

    }

    if (!vm.count("max-ghost-distance"))
    {
        LOG_WARNING << "Using default max ghost distance of 100 m";
    }

    std::vector<std::string> param_filenames;
    for(auto& itr : vm["param-file"].as<std::vector<std::string>>())
    {
        param_filenames.push_back(itr);
    }

    if (vm.count("standalone"))
    {
        LOG_WARNING << "Standalone option enabled. This will not write ghost faces and is intended to write a specific rank as a standalone mesh for debugging.";

    }

    if(MPI_ranks <= 1)
    {
        LOG_ERROR << "Requires ranks >1";
        exit(-1);
    }

    try
    {

        {
            preprocessingTriangulation* tri = new preprocessingTriangulation();
            tri->from_hdf5_and_partition(mesh_filename, param_filenames, MPI_ranks, max_ghost_distance,
                                         standalone_rank);
        }

        // We have input as json and asked for mpi-ranks, so rerun the tool
        if (is_json_mesh && vm.count("mpi-ranks"))
        {
            LOG_DEBUG << "Partitioning mh5 mesh";

            auto h5_mesh_name = boost::filesystem::path(mesh_filename).stem().string() + "_mesh.h5";
            std::vector<std::string> h5_param_name = {boost::filesystem::path(mesh_filename).stem().string() +
                                                      "_param.h5"};

            preprocessingTriangulation* tri = new preprocessingTriangulation();
            tri->from_hdf5_and_partition(h5_mesh_name, h5_param_name, MPI_ranks, max_ghost_distance, standalone_rank);
        }
    }
    catch(exception_base& e)
    {
        LOG_ERROR << boost::diagnostic_information(e);
    }

    LOG_DEBUG << "Done";
}