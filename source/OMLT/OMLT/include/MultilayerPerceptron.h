#pragma once

// std
#include <stdint.h>
#include <vector>

#include "Enums.h"

namespace OMLT
{
	class MultilayerPerceptron
	{
	public:
		~MultilayerPerceptron();
		void FeedForward(float* input_vector, float* output_vector);

		struct Layer
		{
			uint32_t inputs;
			uint32_t outputs;
			ActivationFunction_t function;
			float* biases;
			float** weights;

			Layer(uint32_t in_inputs, uint32_t in_outputs, ActivationFunction_t in_function);
			~Layer();
		};

		Layer* GetLayer(uint32_t index);
		Layer* InputLayer() {return GetLayer(0);}
		Layer* OutputLayer() {return GetLayer(_layers.size() - 1);}

		Layer* PopInputLayer();
		Layer* PopOutputLayer();
		bool AddLayer(Layer*);
		uint32_t LayerCount() const {return _layers.size();}

		// serialization
		std::string ToJSON() const;
		static MultilayerPerceptron* FromJSON(const std::string& in_JSON);
private: 
		std::vector<Layer*> _layers;
		// properly aligned scratch buffers
		std::vector<float*> _accumulations;
		std::vector<float*> _activations;
		// private parse method
		static MultilayerPerceptron* FromJSON(struct cJSON* root);
		friend bool FromJSON(const std::string& in_json, struct Model& out_model);
	};
	typedef MultilayerPerceptron MLP;
}