#include "implicit_part.h"

#include <body.h>

namespace pspy {

ImplicitPart::ImplicitPart(
	const std::string& path, 
	const int N,
	const int N_ref,
	const bool normalize)
{
	valid = true;

	auto bodies = read_file(path);
	if (bodies.size() != 1) {
		valid = false;
		return;
	}

	Body& body = *bodies[0];

	bounding_box = body.GetBoundingBox();

	auto topology = body.GetTopology();

	const int n_faces = topology.faces.size();
	const int n_edges = topology.edges.size();
	const int n_loops = topology.loops.size();

	// Get Samples
	const int N_curve = (int)(ceil(sqrt(N)));

	surface_bounds.resize(n_faces);
	surface_coords.resize(n_faces);
	surface_samples.resize(n_faces);
	for (int i = 0; i < n_faces; ++i) {
		if (!topology.faces[i]->sample_surface(N_ref, N, surface_bounds[i], surface_coords[i], surface_samples[i])) {
			valid = false;
			return;
		}
	}

	curve_bounds.resize(n_edges);
	curve_samples.resize(n_edges);
	for (int i = 0; i < n_edges; ++i) {
		if (!topology.edges[i]->sample_curve(N_curve, curve_bounds[i], curve_samples[i])) {
			valid = false;
			return;
		}
	}

	// Setup Node Data
	const int SURF_PARAM_WIDTH = 11;
	face_surfaces.resize(n_faces);
	face_surface_parameters.resize(n_faces, SURF_PARAM_WIDTH);
	face_surface_parameters.setZero();
	face_surface_flipped.resize(n_faces);
	for (int i = 0; i < n_faces; ++i) {
		face_surfaces[i] = topology.faces[i]->function;
		for (int j = 0; j < topology.faces[i]->parameters.size(); ++j) {
			face_surface_parameters(i, j) = topology.faces[i]->parameters[j];
		}
		face_surface_flipped[i] = (int)(!topology.faces[i]->orientation);
	}

	loop_types.resize(n_loops);
	for (int i = 0; i < n_loops; ++i) {
		loop_types[i] = topology.loops[i]->_type;
	}

	const int CURVE_PARAM_WIDTH = 11;
	edge_curves.resize(n_edges);
	edge_curve_parameters.resize(n_edges, CURVE_PARAM_WIDTH);
	edge_curve_parameters.setZero();
	edge_length.resize(n_edges);
	edge_curve_flipped.resize(n_edges);
	for (int i = 0; i < n_edges; ++i) {
		edge_curves[i] = topology.edges[i]->function;
		for (int j = 0; j < topology.edges[i]->parameters.size(); ++j) {
			edge_curve_parameters(i, j) = topology.edges[i]->parameters[j];
		}
		edge_length[i] = topology.edges[i]->length;
		edge_curve_flipped[i] = topology.edges[i]->_is_reversed;
	}

	vertex_positions.resize(topology.vertices.size(), 3);
	for (int i = 0; i < topology.vertices.size(); ++i) {
		vertex_positions.row(i) = topology.vertices[i]->position;
	}

	// Setup Toploogy Structures
	face_to_loop.resize(2, topology.face_to_loop.size());
	for (int i = 0; i < topology.face_to_loop.size(); ++i) {
		face_to_loop(0, i) = topology.face_to_loop[i]._parent;
		face_to_loop(1, i) = topology.face_to_loop[i]._child;
	}

	loop_to_edge.resize(2, topology.loop_to_edge.size());
	loop_to_edge_flipped.resize(topology.loop_to_edge.size());
	loop_length.resize(n_loops);
	loop_length.setZero();
	for (int i = 0; i < topology.loop_to_edge.size(); ++i) {
		int loop_idx = topology.loop_to_edge[i]._parent;
		int edge_idx = topology.loop_to_edge[i]._child;
		loop_to_edge(0, i) = loop_idx;
		loop_to_edge(1, i) = edge_idx;
		loop_to_edge_flipped[i] = (topology.loop_to_edge[i]._sense == TopoRelationSense::Negative);
		loop_length(loop_idx) += edge_length(edge_idx);
	}

	edge_to_vertex.resize(2, topology.edge_to_vertex.size());
	edge_to_vertex_is_start.resize(topology.edge_to_vertex.size());
	for (int i = 0; i < topology.edge_to_vertex.size(); ++i) {
		int edge_idx = topology.edge_to_vertex[i]._parent;
		int vert_idx = topology.edge_to_vertex[i]._child;
		edge_to_vertex(0, i) = edge_idx;
		edge_to_vertex(1, i) = vert_idx;

		// Very few ~7k edges have only 1 vertex (likely a bug or invalid data?)
		// Parasolid docs indicate it can happen, not sure how though
		// For now, just decide if it is a start vertex or not

		auto s = curve_samples[edge_idx].row(0);
		auto e = curve_samples[edge_idx].row(N_curve - 1);
		Eigen::Vector3d start(s(0), s(1), s(2));
		Eigen::Vector3d end(e(0), e(1), e(2));
		Eigen::Vector3d pos = topology.vertices[vert_idx]->position;
		double start_dist = (start - pos).norm();
		double end_dist = (end - pos).norm();
		edge_to_vertex_is_start[i] = (start_dist <= end_dist);

	}

	face_to_face.resize(3, topology.face_to_face.size());
	for (int i = 0; i < topology.face_to_face.size(); ++i) {
		face_to_face(0, i) = std::get<0>(topology.face_to_face[i]);
		face_to_face(1, i) = std::get<1>(topology.face_to_face[i]);
		face_to_face(2, i) = std::get<2>(topology.face_to_face[i]);
	}

	loop_to_vertex.resize(2, topology.loop_to_vertex.size());
	for (int i = 0; i < topology.loop_to_vertex.size(); ++i) {
		loop_to_vertex(0, i) = topology.loop_to_vertex[i]._parent;
		loop_to_vertex(1, i) = topology.loop_to_vertex[i]._child;
	}

	// Find a canonical ordering of edges for each loop
	ordered_loop_edge.resize(n_loops);
	ordered_loop_flipped.resize(n_loops);
	for (int i = 0; i < loop_to_edge.cols(); ++i) {
		int loop_idx = loop_to_edge(0, i);
		int edge_idx = loop_to_edge(1, i);
		ordered_loop_edge[loop_idx].push_back(edge_idx);
		ordered_loop_flipped[loop_idx].push_back(loop_to_edge_flipped[i]);
	}
	// Iteratively search for next-closest edge. Inefficient but accurate
	/*
	for (int l = 0; l < n_loops; ++l) {
		for (int i = 0; i < ordered_loop_edge[l].size() - 1; ++i) {
			auto curr_end = topology.edges[ordered_loop_edge[l][i]]->end;
			int closest_start_idx = i + 1;
			double closest_dist = (curr_end - 
				topology.edges[ordered_loop_edge[l][closest_start_idx]]->start
				).norm();
			for (int j = i + 1; j < ordered_loop_edge[l].size(); ++j) {
				auto curr_start = topology.edges[ordered_loop_edge[l][j]]->start;
				auto dist = (curr_end - curr_start).norm();
				if (dist < closest_dist) {
					closest_dist = dist;
					closest_start_idx = j;
				}
			}
			// Swap i+1 and closest_start_idx
			int temp_idx = ordered_loop_edge[l][i + 1];
			bool temp_flipped = ordered_loop_flipped[l][i + 1];
			ordered_loop_edge[l][i + 1] = ordered_loop_edge[l][closest_start_idx];
			ordered_loop_flipped[l][i + 1] = ordered_loop_flipped[l][closest_start_idx];
			ordered_loop_edge[l][closest_start_idx] = temp_idx;
			ordered_loop_flipped[l][closest_start_idx] = temp_flipped;
		}
	}
	*/

	scale = 1.0;
	translation = Eigen::RowVector3d(0.0,0.0,0.0);

	if (normalize && valid) {
		// Get Scaling Transformation
		// Translate and scale to fit in [-1,1]^3
		Eigen::RowVector3d min_corner = bounding_box.row(0);
		Eigen::RowVector3d max_corner = bounding_box.row(1);
		Eigen::RowVector3d diag = max_corner - min_corner;
		translation = - (max_corner + min_corner) / 2;
		double max_dim = diag.maxCoeff();
		if (max_dim <= 0) {
			valid = false;
			return;
		}
		scale = 2 / diag.maxCoeff();
		ApplyTransform(translation, scale);
	}
}

void ImplicitPart::ApplyTransform(
	const Eigen::RowVector3d& translation, 
	const double scale
)
{
	const int n_faces = face_surfaces.size();
	const int n_edges = edge_curves.size();

	// Apply to surface parameters
	// cols 0-2 translate and scale (origin x,y,z)
	// col 9 just scales (radius where applicable) !!!! -- BUG -- not currently being scaled!
	// col 10 just scales, but only for torus (2nd radius)

	face_surface_parameters.block(0, 0, n_faces, 3).rowwise() += translation;
	face_surface_parameters.block(0, 0, n_faces, 3) *= scale;
	//face_surface_parameters.block(0, 9, n_faces, 1) *= scale;
	for (int i = 0; i < n_faces; ++i) {
		if (face_surfaces[i] == SurfaceFunction::TORUS) {
			face_surface_parameters(i, 10) *= scale;
		}
	}	


	// Apply to edge parameters
	// cols 0-2 translate and scale (origin x,y,z)
	// cols 9-10 scale (radii)

	edge_curve_parameters.block(0, 0, n_edges, 3).rowwise() += translation;
	edge_curve_parameters.block(0, 0, n_edges, 3) *= scale;
	edge_curve_parameters.block(0, 9, n_edges, 2) *= scale;

	// Apply to vertex positions
	vertex_positions.rowwise() += translation;
	vertex_positions *= scale;

	// Apply to samples
	// cols 0-3 translate and scale (x,y,z)
	for (int i = 0; i < n_faces; ++i) {
		const int N = surface_samples[i].rows();
		surface_samples[i].block(0, 0, N, 3).rowwise() += translation;
		surface_samples[i].block(0, 0, N, 3) *= scale;
	}
	for (int i = 0; i < n_edges; ++i) {
		const int N = curve_samples[i].rows();
		curve_samples[i].block(0, 0, N, 3).rowwise() += translation;
		curve_samples[i].block(0, 0, N, 3) *= scale;
	}

	// Apply to bounds
	// Only scale non-periodic dimensions
	for (int i = 0; i < n_faces; ++i) {
		switch (face_surfaces[i]) {
		case SurfaceFunction::PLANE:
			surface_bounds[i] *= scale; // scale u and v
			break;
		case SurfaceFunction::CYLINDER:
			surface_bounds[i].col(1) *= scale; // scale v
			break;
		case SurfaceFunction::CONE:
			surface_bounds[i].col(1) *= scale; // scale v
			break;
		case SurfaceFunction::SPHERE:
			break;
		case SurfaceFunction::TORUS:
			break;
		}
	}

	for (int i = 0; i < n_edges; ++i) {
		if (edge_curves[i] == CurveFunction::LINE) {
			curve_bounds[i] *= scale;
		}
	}

	// Apply to edge and loop lengths
	edge_length *= scale;
	loop_length *= scale;

	// Apply to bounding box
	bounding_box.rowwise() += translation;
	bounding_box *= scale;
}

}